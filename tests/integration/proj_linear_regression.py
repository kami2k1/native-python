# Real project: machine learning — linear regression via gradient descent,
# fitting y = 2x + 1 exactly. Prints learned parameters (x100, rounded).
xs = []
ys = []
for i in range(10):
    xs.append(float(i))
    ys.append(2.0 * i + 1.0)

w = 0.0
b = 0.0
lr = 0.01
n = float(len(xs))
for epoch in range(5000):
    dw = 0.0
    db = 0.0
    for i in range(10):
        pred = w * xs[i] + b
        err = pred - ys[i]
        dw += 2.0 * err * xs[i] / n
        db += 2.0 * err / n
    w -= lr * dw
    b -= lr * db

print(int(w * 100.0 + 0.5))
print(int(b * 100.0 + 0.5))
mse = 0.0
for i in range(10):
    d = w * xs[i] + b - ys[i]
    mse += d * d
print(mse < 0.0001)
