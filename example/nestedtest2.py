def factories():
    funcs = []
    for i in range(3):
        def f():
            return i
        funcs.append(f)
    return funcs


fs = factories()
print([f() for f in fs])


def outer(a, b):
    def inner(x):
        return a + b + x
    return inner


g = outer(1, 2)
print(g(3))


def decorated_factory():
    def show(fn):
        return lambda *a: "[" + str(fn(*a)) + "]"
    @show
    def value(x):
        return x * 2
    return value


h = decorated_factory()
print(h(4))
print("done")
