def kw(a, b=2, *rest, c=3, **opts):
    return (a, b, rest, c, sorted(opts.items()))

print(kw(1))
print("after1")
print(kw(1, 5, 6, 7, c=8, d=9))
print("after2")
print(kw(a=1, c=4))
print("after3")
