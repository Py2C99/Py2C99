import functools
import itertools

print(functools.reduce(lambda a, b: a + b, [1, 2, 3, 4]))
print(list(itertools.chain([1, 2], [3, 4])))
print(list(itertools.repeat(7, 3)))
print(list(itertools.product([1, 2], [3, 4])))
print("done")
