import math


def fib(n):
    a, b = 0, 1
    out = []
    for i in range(n):
        out.append(a)
        a, b = b, a + b
    return out


class Greeter:
    def __init__(self, name):
        self.name = name
        self.count = 0

    def greet(self, who=None):
        self.count += 1
        if who is None:
            who = "world"
        return "Hello, %s! (%s #%d)" % (who, self.name, self.count)


def main():
    print("fib:", fib(10))
    g = Greeter("py2c99")
    print(g.greet())
    print(g.greet("C99"))
    print("sqrt(2) =", math.sqrt(2.0))
    total = 0
    for x in [1, 2, 3, 4, 5]:
        if x % 2 == 0:
            continue
        total += x
    print("odd total:", total)
    squares = [i * i for i in range(6) if i % 2 == 0]
    print("squares:", squares)
    d = {"a": 1, "b": 2}
    print("dict:", d, "keys:", list(d.keys()))
    try:
        raise ValueError("boom")
    except ValueError as e:
        print("caught:", e)
    finally:
        print("finally ran")
    print(f"fstring {total} {g.name!r}")


main()
