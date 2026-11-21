x = 100


def outer():
    x = 10

    def middle():
        def inner():
            global x
            return x
        return inner()
    return middle()


print(outer())


def deep():
    a = 1

    def mid():
        def inner():
            nonlocal a
            a += 5
            return a
        return inner()
    return mid(), a


print(deep())
print("done")
