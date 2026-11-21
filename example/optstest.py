x = 0
if True:
    x = 1
if False:
    x = 2
else:
    x = 3
print(x)

y = 10 if 1 + 2 == 3 else 20
print(y)

n = 0
while False:
    n = 99
else:
    n = 5
print(n)

k = 0
while True:
    k += 1
    if k >= 3:
        break
print(k)

print("done")
