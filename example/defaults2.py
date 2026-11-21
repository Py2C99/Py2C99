def make(x):
    def f(y=x):
        return y
    return f


a = make(1)
b = make(2)
print(a())
print(b())


def lam_factory(v):
    return lambda w=v: v + w


g = lam_factory(10)
h = lam_factory(20)
print(g())
print(h())
print("done")
