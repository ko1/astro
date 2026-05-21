# graph_bfs — repeated BFS over a long-lived adjacency-list graph.
#
# Macro pattern with clear gen-GC behavior:
#   - Long-lived: 10k-node graph as Array of [id, neighbors_array].
#     Built once at top, lives the whole bench → promoted to old gen
#     after first few minors.
#   - Per-BFS: queue (FIFO via index) + visited array (10k bools) +
#     dist array (10k ints).  All allocated fresh, all short-lived,
#     fit comfortably in nursery for any reasonable nursery size.
#
# Stresses:
#   - For gen backends: nursery throughput (each BFS allocates ~40 KB
#     of arrays, runs hot loop reading from long-lived neighbors).
#   - Old-gen scan cost: every collection has to skip 10k tenured cells
#     even though none of them is mutated → tests generational ROI.
#   - For non-gen backends: the full graph is re-scanned every GC,
#     making this a fair "non-gen pays for old-gen size" test.
#   - Write barrier: zero (we never mutate the graph after build).
#     Pure "read-only old" workload — gen should win cleanly here
#     since there's no remset traffic.

def build_graph(n)
  g = []
  i = 0
  while i < n
    # Deterministic ~5-neighbor adjacency.  Edges wrap around.
    nbrs = []
    j = 1
    while j <= 5
      nbrs.push((i + j * 13) % n)
      j = j + 1
    end
    g.push([i, nbrs])
    i = i + 1
  end
  g
end

def bfs(g, start)
  n = g.size
  visited = []
  dist = []
  i = 0
  while i < n
    visited.push(false)
    dist.push(-1)
    i = i + 1
  end

  # Queue as Array used FIFO via head index — avoids shift O(n).
  queue = [start]
  head = 0
  visited[start] = true
  dist[start] = 0

  total = 0
  while head < queue.size
    u = queue[head]
    head = head + 1
    total = total + dist[u]
    node = g[u]
    nbrs = node[1]
    k = 0
    nk = nbrs.size
    while k < nk
      v = nbrs[k]
      if visited[v] == false
        visited[v] = true
        dist[v] = dist[u] + 1
        queue.push(v)
      end
      k = k + 1
    end
  end
  total
end

# Build the graph once (long-lived).
n_nodes = 10_000
g = build_graph(n_nodes)

# Many BFSs from different sources.  Each BFS is independent and its
# working set (visited/dist/queue) is short-lived.
n_bfs = 240
acc = 0
s = 0
while s < n_bfs
  # Start node varies to avoid identical traversals.
  total = bfs(g, (s * 137) % n_nodes)
  acc = acc + total
  s = s + 1
end

p acc
