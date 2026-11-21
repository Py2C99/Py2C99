def twice(fn):
    return lambda x: fn(fn(x))


@twice
def inc(v):
    return v + 1


print(inc(5))


def shout(fn):
    return lambda v: str(fn(v)) + "!"


@shout
def sq(v):
    return v * v


print(sq(4))
print("done")
