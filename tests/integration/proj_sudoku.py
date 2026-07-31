# Real project: sudoku solver (backtracking, 2D lists, deep recursion).
puzzle = []
puzzle.append("530070000")
puzzle.append("600195000")
puzzle.append("098000060")
puzzle.append("800060003")
puzzle.append("400803001")
puzzle.append("700020006")
puzzle.append("060000280")
puzzle.append("000419005")
puzzle.append("000080079")

grid = []
for r in range(9):
    row = []
    for c in range(9):
        row.append(ord(puzzle[r][c]) - 48)
    grid.append(row)

def valid(g, r, c, v):
    for i in range(9):
        if g[r][i] == v:
            return False
        if g[i][c] == v:
            return False
    br = (r // 3) * 3
    bc = (c // 3) * 3
    for i in range(3):
        for j in range(3):
            if g[br + i][bc + j] == v:
                return False
    return True

def solve(g, pos):
    if pos == 81:
        return True
    r = pos // 9
    c = pos % 9
    if g[r][c] != 0:
        return solve(g, pos + 1)
    for v in range(1, 10):
        if valid(g, r, c, v):
            g[r][c] = v
            if solve(g, pos + 1):
                return True
            g[r][c] = 0
    return False

if solve(grid, 0):
    for r in range(9):
        line = ""
        for c in range(9):
            line += str(grid[r][c])
        print(line)
else:
    print("no solution")
