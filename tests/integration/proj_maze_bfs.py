# Real project: BFS shortest path in a grid maze (queue, visited dict).
maze = []
maze.append("###########")
maze.append("#S..#.....#")
maze.append("#.#.#.###.#")
maze.append("#.#...#...#")
maze.append("#.#####.###")
maze.append("#.....#...#")
maze.append("###.#.###.#")
maze.append("#...#....E#")
maze.append("###########")

H = len(maze)
W = len(maze[0])
start = 0
goal = 0
for r in range(H):
    for c in range(W):
        if maze[r][c] == "S":
            start = r * W + c
        if maze[r][c] == "E":
            goal = r * W + c

queue = [start]
dist = {start: 0}
head = 0
dr = [1, -1, 0, 0]
dc = [0, 0, 1, -1]
while head < len(queue):
    cur = queue[head]
    head += 1
    if cur == goal:
        break
    r = cur // W
    c = cur % W
    for k in range(4):
        nr = r + dr[k]
        nc = c + dc[k]
        if nr >= 0 and nr < H and nc >= 0 and nc < W:
            if maze[nr][nc] != "#":
                key = nr * W + nc
                if not (key in dist):
                    dist[key] = dist[cur] + 1
                    queue.append(key)

if goal in dist:
    print("shortest path:", dist[goal])
else:
    print("unreachable")
print("visited:", len(dist))
