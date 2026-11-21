import bisect
xs = [1, 2, 3]
print(bisect.bisect_left(xs, 0), bisect.bisect_left(xs, 2), bisect.bisect_left(xs, 4))
print(bisect.bisect_right(xs, 0), bisect.bisect_right(xs, 2), bisect.bisect_right(xs, 4))
ys = [1, 2, 3]
bisect.insort(ys, 4)
print(ys)
zs = [1, 2, 3]
bisect.insort(zs, 0)
print(zs)
