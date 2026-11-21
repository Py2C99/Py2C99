import bisect
xs = [1, 2, 3]
print(bisect.bisect_left(xs, 2))
print(bisect.bisect_right(xs, 2))
print(xs)
bisect.insort(xs, 2)
print(xs)
