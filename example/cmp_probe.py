import operator
xs = [1, 2, 3]
print(len(xs), xs)
print(operator.lt(2, 1), operator.lt(2, 2), operator.lt(2, 3))
print(operator.contains(xs, 2))
