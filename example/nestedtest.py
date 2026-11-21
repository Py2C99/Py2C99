def make_adder(n):
    def add(x):
        return x + n
    return add


add5 = make_adder(5)
print(add5(10))


def counter():
    count = 0

    def inc():
        nonlocal count
        count += 1
        return count
    return inc


c = counter()
print(c())
print(c())
print(c())


def outer():
    msg = "hi"

    def inner(a=1):
        return msg + str(a)
    return inner


f = outer()
print(f())
print(f(7))
print("done")
