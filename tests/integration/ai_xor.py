# "AI" test: a tiny neural network (2-3-1, sigmoid, backprop) written in pure
# KamiPython, trained on XOR, compiled to a native binary. Deterministic init.
import math

def sigmoid(x):
    return 1.0 / (1.0 + math.exp(-x))

# 2 inputs -> 3 hidden -> 1 output
w1 = [[0.5, -0.4], [0.3, 0.8], [-0.6, 0.2]]
b1 = [0.1, -0.2, 0.05]
w2 = [0.7, -0.5, 0.4]
b2 = 0.0

inputs = [[0.0, 0.0], [0.0, 1.0], [1.0, 0.0], [1.0, 1.0]]
targets = [0.0, 1.0, 1.0, 0.0]

lr = 0.8
for epoch in range(6000):
    for s in range(4):
        x = inputs[s]
        t = targets[s]
        # forward
        h = [0.0, 0.0, 0.0]
        for j in range(3):
            z = b1[j]
            for i in range(2):
                z += w1[j][i] * x[i]
            h[j] = sigmoid(z)
        zo = b2
        for j in range(3):
            zo += w2[j] * h[j]
        o = sigmoid(zo)
        # backward (squared error)
        do = (o - t) * o * (1.0 - o)
        for j in range(3):
            dh = do * w2[j] * h[j] * (1.0 - h[j])
            w2[j] -= lr * do * h[j]
            for i in range(2):
                w1[j][i] -= lr * dh * x[i]
            b1[j] -= lr * dh
        b2 -= lr * do

# evaluate: must have learned XOR
for s in range(4):
    x = inputs[s]
    h = [0.0, 0.0, 0.0]
    for j in range(3):
        z = b1[j]
        for i in range(2):
            z += w1[j][i] * x[i]
        h[j] = sigmoid(z)
    zo = b2
    for j in range(3):
        zo += w2[j] * h[j]
    o = sigmoid(zo)
    if o > 0.5:
        print(1)
    else:
        print(0)

print("xor trained")
