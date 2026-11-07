#!/usr/bin/env python3
"""Generate quiet random positions with a fixed seed (no decided tactic).

Rule: stones are dropped alternately with a fixed RNG seed into empty cells
within Chebyshev distance 2 of an existing stone (so the cluster grows like a
real opening).  A position is rejected when either side already has four or
more in a row on any line, or when a side has three with both ends open
(these would make the position tactical rather than "quiet").

Output: corpus-quiet.json, same schema as corpus.json (games[].snaps[]).
"""
import json, os, random

ROOT = os.path.dirname(os.path.abspath(__file__))
SEED = 20261107
N = 15
DIRS = [(1, 0), (0, 1), (1, 1), (1, -1)]


def runs(board, x, y, dx, dy, color):
    """Consecutive count of `color` through (x,y) plus open-end flags."""
    # walk backwards then forwards along the direction
    bx, by = x - dx, y - dy
    cnt = 1
    while True:
        nx, ny = bx, by
        if not (0 <= nx < N and 0 <= ny < N) or board[ny][nx] != color:
            break
        cnt += 1
        bx -= dx
        by -= dy
    open_back = 0 <= bx < N and 0 <= by < N and board[by][bx] == 0
    fx, fy = x + dx, y + dy
    while True:
        nx, ny = fx, fy
        if not (0 <= nx < N and 0 <= ny < N) or board[ny][nx] != color:
            break
        cnt += 1
        fx += dx
        fy += dy
    open_front = 0 <= fx < N and 0 <= fy < N and board[fy][fx] == 0
    return cnt, open_back, open_front


def quiet(board):
    for y in range(N):
        for x in range(N):
            c = board[y][x]
            if not c:
                continue
            for dx, dy in DIRS:
                cnt, ob, of = runs(board, x, y, dx, dy, c)
                if cnt >= 4:
                    return False
                if cnt == 3 and ob and of:
                    return False
    return True


def gen(stones, rng):
    while True:
        board = [[0] * N for _ in range(N)]
        board[7][7] = 1
        moves = [[7, 7, 1]]
        color = 2
        for _ in range(stones - 1):
            cells = [(x, y) for y in range(N) for x in range(N)
                     if board[y][x] == 0 and any(
                         0 <= px < N and 0 <= py < N and board[py][px]
                         for px in (x - 2, x - 1, x, x + 1, x + 2)
                         for py in (y - 2, y - 1, y, y + 1, y + 2))]
            x, y = rng.choice(cells)
            board[y][x] = color
            moves.append([x, y, color])
            color = 3 - color
        if quiet(board):
            return moves


def main():
    rng = random.Random(SEED)
    games = []
    for i, stones in enumerate([8, 8, 10, 10]):
        moves = gen(stones, rng)
        games.append({'seed': [[7, 7, 1]], 'stones': stones,
                      'snaps': [{'stones': stones, 'moves': moves}]})
    out = {'exe': 'random-quiet', 'seed': SEED, 'rule': 2, 'games': games}
    path = os.path.join(ROOT, 'corpus-quiet.json')
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    for g in games:
        print('stones=%d moves=%s' % (g['stones'],
              ';'.join('%d,%d,%d' % tuple(m) for m in g['snaps'][0]['moves'])))
    print('wrote ' + path)


if __name__ == '__main__':
    main()
