#include "AlgorithmEngine.h"
#include <algorithm>
#include <numeric>
#include <random>
#include <chrono>
#include <limits>
#include <functional>
#include <cmath>

static constexpr double INF = std::numeric_limits<double>::max();

// ─── Communication cost ───────────────────────────────────────────────────────
double AlgorithmEngine::commCost(const DAGData& dag,
                                  int predTask, int predVm,
                                  int /*succTask*/, int succVm)
{
    if (predVm == succVm) return 0.0;
    const auto& et = dag.tasks[predTask].execTimes;
    double avg = 0.0; for (double v : et) avg += v;
    avg /= et.size();
    return dag.commCostFactor * avg;
}

// ─── EFT ─────────────────────────────────────────────────────────────────────
double AlgorithmEngine::computeEFT(const DAGData& dag, int taskId, int vmId,
                                    const QVector<double>& vmReady,
                                    const QMap<int,ScheduleEntry>& scheduled,
                                    double& estOut)
{
    double est = vmReady[vmId];
    for (int p : dag.tasks[taskId].predecessors) {
        if (!scheduled.contains(p)) continue;
        const auto& se = scheduled[p];
        est = std::max(est, se.finishTime + commCost(dag, p, se.vmId, taskId, vmId));
    }
    estOut = est;
    return est + dag.tasks[taskId].execTimes[vmId];
}

// ─── Topological sort (Kahn) ──────────────────────────────────────────────────
QVector<int> AlgorithmEngine::topologicalSort(const DAGData& dag)
{
    int n = dag.tasks.size();
    QVector<int> indeg(n, 0);
    for (const auto& t : dag.tasks) for (int s : t.successors) indeg[s]++;
    QVector<int> q, res;
    for (int i = 0; i < n; i++) if (!indeg[i]) q.push_back(i);
    while (!q.empty()) {
        std::sort(q.begin(), q.end());
        int cur = q.front(); q.erase(q.begin()); res.push_back(cur);
        for (int s : dag.tasks[cur].successors) if (!--indeg[s]) q.push_back(s);
    }
    return res;
}

// ─── Topological levels ───────────────────────────────────────────────────────
QVector<QVector<int>> AlgorithmEngine::computeLevels(const DAGData& dag)
{
    int n = dag.tasks.size();
    QVector<int> lvl(n, 0);
    for (int id : topologicalSort(dag))
        for (int s : dag.tasks[id].successors)
            lvl[s] = std::max(lvl[s], lvl[id] + 1);
    int mx = *std::max_element(lvl.begin(), lvl.end());
    QVector<QVector<int>> layers(mx + 1);
    for (int i = 0; i < n; i++) layers[lvl[i]].push_back(i);
    return layers;
}

// ─── Upward rank ──────────────────────────────────────────────────────────────
QVector<double> AlgorithmEngine::computeUpwardRank(const DAGData& dag)
{
    int n = dag.tasks.size();
    QVector<double> rank(n, -1.0);
    auto avg = [&](int id){ double s=0; for(double v:dag.tasks[id].execTimes) s+=v; return s/dag.tasks[id].execTimes.size(); };
    std::function<double(int)> calc = [&](int id) -> double {
        if (rank[id] >= 0) return rank[id];
        double best = 0;
        for (int s : dag.tasks[id].successors)
            best = std::max(best, commCost(dag,id,0,s,1) + calc(s));
        return rank[id] = avg(id) + best;
    };
    for (int i = 0; i < n; i++) calc(i);
    return rank;
}

// ─── Downward rank ────────────────────────────────────────────────────────────
QVector<double> AlgorithmEngine::computeDownwardRank(const DAGData& dag)
{
    int n = dag.tasks.size();
    QVector<double> rank(n, -1.0);
    auto avg = [&](int id){ double s=0; for(double v:dag.tasks[id].execTimes) s+=v; return s/dag.tasks[id].execTimes.size(); };
    std::function<double(int)> calc = [&](int id) -> double {
        if (rank[id] >= 0) return rank[id];
        double best = 0;
        for (int p : dag.tasks[id].predecessors)
            best = std::max(best, calc(p) + avg(p) + commCost(dag,p,0,id,1));
        return rank[id] = best;
    };
    for (int i = 0; i < n; i++) calc(i);
    return rank;
}

// ─── Critical path tracing ────────────────────────────────────────────────────
QVector<int> AlgorithmEngine::traceCriticalPath(const AlgorithmResult& res, const DAGData& dag)
{
    QMap<int,ScheduleEntry> sched;
    for (const auto& se : res.entries) sched[se.taskId] = se;
    // Find exit task (max finish)
    int exitTask = -1; double maxFT = -1;
    for (auto it = sched.begin(); it != sched.end(); ++it)
        if (it->finishTime > maxFT) { maxFT = it->finishTime; exitTask = it->taskId; }
    // Trace back
    QVector<int> path;
    int cur = exitTask;
    while (cur >= 0) {
        path.prepend(cur);
        const auto& se = sched[cur];
        int critPred = -1; double maxContrib = -1;
        for (int p : dag.tasks[cur].predecessors) {
            if (!sched.contains(p)) continue;
            const auto& pse = sched[p];
            double cc = commCost(dag, p, pse.vmId, cur, se.vmId);
            double contrib = pse.finishTime + cc;
            if (contrib > maxContrib) { maxContrib = contrib; critPred = p; }
        }
        cur = critPred;
    }
    return path;
}

// ═══ HEFT (standard) ══════════════════════════════════════════════════════════
AlgorithmResult AlgorithmEngine::runHEFT(const DAGData& dag)
{
    auto t0 = std::chrono::high_resolution_clock::now();
    int n = dag.tasks.size(), m = dag.numVMs();
    auto upRank = computeUpwardRank(dag);
    QVector<int> priority(n); std::iota(priority.begin(), priority.end(), 0);
    std::sort(priority.begin(), priority.end(), [&](int a, int b){ return upRank[a]>upRank[b]; });

    QVector<double> vmReady(m, 0.0);
    QMap<int,ScheduleEntry> scheduled;
    for (int tid : priority) {
        int bestVM=0; double bestEFT=INF, bestEST=0;
        for (int v = 0; v < m; v++) {
            double est=0, eft=computeEFT(dag,tid,v,vmReady,scheduled,est);
            if (eft<bestEFT) { bestEFT=eft; bestVM=v; bestEST=est; }
        }
        scheduled[tid] = {tid, bestVM, bestEST, bestEFT};
        vmReady[bestVM] = bestEFT;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    AlgorithmResult res;
    res.algorithmName = "HEFT";
    res.algorithmDesc = "Heterogeneous Earliest Finish Time — greedy upward-rank priority";
    res.numVMs=m; res.numTasks=n;
    for (auto& se : scheduled) res.entries.push_back(se);
    res.makespan = *std::max_element(vmReady.begin(), vmReady.end());
    res.runtimeMs = std::chrono::duration<double,std::milli>(t1-t0).count();
    res.criticalPath = traceCriticalPath(res, dag);
    res.isValid = true;
    return res;
}

// ═══ HEFT + Dynamic Programming (level-wise optimal assignment) ═══════════════
AlgorithmResult AlgorithmEngine::runHEFT_DP(const DAGData& dag)
{
    auto t0 = std::chrono::high_resolution_clock::now();
    int n = dag.tasks.size(), m = dag.numVMs();
    auto upRank = computeUpwardRank(dag);
    auto levels = computeLevels(dag);

    QVector<double> vmReady(m, 0.0);
    QMap<int,ScheduleEntry> scheduled;

    for (auto& layer : levels) {
        // Sort layer by decreasing upward rank
        std::sort(layer.begin(), layer.end(), [&](int a,int b){ return upRank[a]>upRank[b]; });
        int ln = layer.size();

        if (ln <= 10) {
            // ── Bitmask DP: find assignment minimising max finish time ──────────
            // State: dp[mask] = VM ready times snapshot that minimises max(vmReady)
            // We store best VM assignment per task
            struct State { QVector<double> vmR; double maxFT; QVector<int> assign; };
            QVector<State> dp(1<<ln);
            dp[0].vmR = vmReady;
            dp[0].maxFT = *std::max_element(vmReady.begin(),vmReady.end());
            dp[0].assign.resize(ln, -1);

            for (int mask = 0; mask < (1<<ln); mask++) {
                if (dp[mask].vmR.isEmpty()) continue;
                // find lowest unset bit = next task index in layer
                int ti = -1;
                for (int b = 0; b < ln; b++) if (!(mask>>b&1)){ ti=b; break; }
                if (ti < 0) continue;
                int tid = layer[ti];
                int newMask = mask|(1<<ti);
                for (int v = 0; v < m; v++) {
                    double est=0, eft=computeEFT(dag,tid,v,dp[mask].vmR,scheduled,est);
                    if (dp[newMask].vmR.isEmpty() || eft < dp[newMask].vmR[v]) {
                        auto newVmR = dp[mask].vmR;
                        newVmR[v] = eft;
                        double mft = *std::max_element(newVmR.begin(),newVmR.end());
                        if (dp[newMask].vmR.isEmpty() || mft < dp[newMask].maxFT) {
                            dp[newMask].vmR = newVmR;
                            dp[newMask].maxFT = mft;
                            dp[newMask].assign = dp[mask].assign;
                            dp[newMask].assign[ti] = v;
                        }
                    }
                }
            }
            // Apply best assignment
            int fullMask = (1<<ln)-1;
            auto& best = dp[fullMask];
            // Reconstruct entries in task order (respecting actual EST)
            for (int ti = 0; ti < ln; ti++) {
                int tid = layer[ti], v = best.assign[ti];
                if (v < 0) v = 0;
                double est=0, eft=computeEFT(dag,tid,v,vmReady,scheduled,est);
                scheduled[tid]={tid,v,est,eft};
                vmReady[v] = std::max(vmReady[v], eft);
            }
        } else {
            // ── LPT greedy for large layers ─────────────────────────────────────
            for (int tid : layer) {
                int bestVM=0; double bestEFT=INF, bestEST=0;
                for (int v = 0; v < m; v++) {
                    double est=0, eft=computeEFT(dag,tid,v,vmReady,scheduled,est);
                    if (eft<bestEFT){ bestEFT=eft; bestVM=v; bestEST=est; }
                }
                scheduled[tid]={tid,bestVM,bestEST,bestEFT};
                vmReady[bestVM]=bestEFT;
            }
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    AlgorithmResult res;
    res.algorithmName = "HEFT + Dynamic Programming";
    res.algorithmDesc = "Optimal VM assignment per topological level via bitmask DP";
    res.numVMs=m; res.numTasks=n;
    for (auto& se : scheduled) res.entries.push_back(se);
    res.makespan = *std::max_element(vmReady.begin(), vmReady.end());
    res.runtimeMs = std::chrono::duration<double,std::milli>(t1-t0).count();
    res.criticalPath = traceCriticalPath(res, dag);
    res.isValid = true;
    return res;
}

// ═══ Cluster assignment helper ════════════════════════════════════════════════
QVector<int> AlgorithmEngine::clusterTasks(const DAGData& dag)
{
    int n = dag.tasks.size(), k = dag.numVMs();
    // Build affinity: affinity[i][j] = number of shared edges
    QVector<int> cluster(n, -1);
    // Assign entry tasks round-robin, then propagate by majority
    auto order = topologicalSort(dag);
    int nextCluster = 0;
    for (int tid : order) {
        if (cluster[tid] >= 0) continue;
        if (dag.tasks[tid].predecessors.isEmpty()) {
            cluster[tid] = nextCluster++ % k;
        } else {
            // Majority vote from predecessors
            QVector<int> votes(k, 0);
            for (int p : dag.tasks[tid].predecessors)
                if (cluster[p] >= 0) votes[cluster[p]]++;
            cluster[tid] = (int)(std::max_element(votes.begin(),votes.end()) - votes.begin());
        }
    }
    return cluster;
}

// ═══ HEFT + Divide & Conquer ══════════════════════════════════════════════════
AlgorithmResult AlgorithmEngine::runHEFT_DC(const DAGData& dag)
{
    auto t0 = std::chrono::high_resolution_clock::now();
    int n = dag.tasks.size(), m = dag.numVMs();
    auto upRank = computeUpwardRank(dag);
    auto taskCluster = clusterTasks(dag); // cluster[taskId] = preferred VM

    // Priority = upward rank (same as HEFT)
    QVector<int> priority(n); std::iota(priority.begin(), priority.end(), 0);
    std::sort(priority.begin(), priority.end(), [&](int a,int b){ return upRank[a]>upRank[b]; });

    QVector<double> vmReady(m, 0.0);
    QMap<int,ScheduleEntry> scheduled;

    for (int tid : priority) {
        int preferredVM = taskCluster[tid];
        // Try preferred VM first, then others
        QVector<int> vmOrder(m);
        std::iota(vmOrder.begin(), vmOrder.end(), 0);
        std::stable_sort(vmOrder.begin(), vmOrder.end(), [&](int a, int b){
            if (a == preferredVM) return true;
            if (b == preferredVM) return false;
            // Sort remaining by proximity to preferred (wrap-around)
            int da = std::abs(a - preferredVM), db = std::abs(b - preferredVM);
            return da < db;
        });

        int bestVM=preferredVM; double bestEFT=INF, bestEST=0;
        // Give preferred VM a 15% bonus to bias towards cluster cohesion
        for (int v : vmOrder) {
            double est=0, eft=computeEFT(dag,tid,v,vmReady,scheduled,est);
            double score = (v == preferredVM) ? eft * 0.85 : eft;
            if (score < bestEFT) { bestEFT=eft; bestVM=v; bestEST=est; }
        }
        scheduled[tid]={tid,bestVM,bestEST,bestEFT};
        vmReady[bestVM]=bestEFT;
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    AlgorithmResult res;
    res.algorithmName = "HEFT + Divide & Conquer";
    res.algorithmDesc = "Cluster tasks by DAG affinity; prioritise intra-cluster communication";
    res.numVMs=m; res.numTasks=n;
    for (auto& se : scheduled) res.entries.push_back(se);
    res.makespan = *std::max_element(vmReady.begin(), vmReady.end());
    res.runtimeMs = std::chrono::duration<double,std::milli>(t1-t0).count();
    res.criticalPath = traceCriticalPath(res, dag);
    res.isValid = true;
    return res;
}

// ═══ DAG Generation (random, variable size) ═══════════════════════════════════
DAGData AlgorithmEngine::generateDAG(int numTasks, int numVMs,
                                      double commFactor, unsigned seed)
{
    std::mt19937 rng(seed);
    auto randExec = [&](double speedFactor) {
        std::uniform_real_distribution<double> d(5.0, 20.0);
        return d(rng) / speedFactor;
    };

    DAGData dag;
    dag.commCostFactor = commFactor;
    dag.label = QString("Random (%1 tasks, %2 VMs)").arg(numTasks).arg(numVMs);

    // Build VMs with varying speeds
    static const double speeds[] = {1.0, 0.8, 1.2, 0.9, 1.5, 0.7, 1.1, 1.3};
    dag.vms.resize(numVMs);
    for (int v = 0; v < numVMs; v++) {
        dag.vms[v] = {v, QString("VM %1").arg(v+1), speeds[v % 8]};
    }

    // Build tasks
    dag.tasks.resize(numTasks);
    for (int i = 0; i < numTasks; i++) {
        dag.tasks[i].id   = i;
        dag.tasks[i].name = QString("T%1").arg(i);
        dag.tasks[i].execTimes.resize(numVMs);
        for (int v = 0; v < numVMs; v++)
            dag.tasks[i].execTimes[v] = randExec(dag.vms[v].speedFactor);
    }

    // Build edges: layered DAG to guarantee no cycles
    int numLayers = std::max(3, numTasks / 4);
    QVector<QVector<int>> layers(numLayers);
    std::uniform_int_distribution<int> layerDist(0, numLayers-2);
    layers[0].push_back(0);
    for (int i = 1; i < numTasks-1; i++) layers[layerDist(rng)+1].push_back(i);
    layers[numLayers-1].push_back(numTasks-1);

    // Remove empty layers
    layers.erase(std::remove_if(layers.begin(), layers.end(),
                                [](const QVector<int>& l){ return l.isEmpty(); }), layers.end());

    // Add edges: each task connects to 1-3 tasks in the next layer
    std::uniform_int_distribution<int> edgeDist(1, 3);
    for (int li = 0; li+1 < layers.size(); li++) {
        for (int src : layers[li]) {
            auto& nextLayer = layers[li+1];
            int numEdges = std::min(edgeDist(rng), (int)nextLayer.size());
            // Shuffle next layer and pick first numEdges
            QVector<int> targets = nextLayer;
            std::shuffle(targets.begin(), targets.end(), rng);
            for (int e = 0; e < numEdges; e++) {
                int dst = targets[e];
                // Avoid duplicate edges
                if (!dag.tasks[src].successors.contains(dst)) {
                    dag.tasks[src].successors.push_back(dst);
                    dag.tasks[dst].predecessors.push_back(src);
                }
            }
        }
    }
    // Ensure last layer task connects to entry if not already reached
    // Ensure every task is reachable: tasks with no preds in non-first layers
    for (int li = 1; li < layers.size(); li++) {
        for (int tid : layers[li]) {
            if (dag.tasks[tid].predecessors.isEmpty() && !layers[li-1].isEmpty()) {
                int src = layers[li-1][0];
                dag.tasks[src].successors.push_back(tid);
                dag.tasks[tid].predecessors.push_back(src);
            }
        }
    }

    return dag;
}
