import math
import os
from math import sqrt, pi

print("==== functions ====")


def greet(name, punct="!"):
    return f"Hello {name}{punct}"


def add_all(*nums, **opts):
    total = sum(nums)
    if "double" in opts and opts["double"]:
        total *= 2
    return total


def outer():
    x = 10
    f = lambda y: x + y
    x = 20
    return f(5)


print(greet("a"), greet("b", "?"))
print(add_all(1, 2, 3), add_all(1, 2, double=True))
print(outer())
print(sqrt(9), pi)

print("==== classes ====")


class Animal:
    sound = "?"

    def __init__(self, name):
        self.name = name

    def speak(self):
        return self.name + " says " + self.sound


class Dog(Animal):
    sound = "woof"

    def __init__(self, name, age=1):
        Animal.__init__(self, name)
        self.age = age


d = Dog("Rex", 3)
print(d.speak(), d.age)
print(Dog("Pup").age)

print("==== comprehensions ====")

nums = [1, 2, 3, 4, 5]
print([n * n for n in nums if n % 2])
print({n: n * n for n in nums})
print(sorted(list(set(nums)), reverse=True))
print(list(enumerate("abc")))
print(list(zip(nums[:3], "xyz")))
print(any(n > 4 for n in nums), all(n > 0 for n in nums))

print("==== loops ====")

for i in nums:
    if i == 3:
        break
else:
    print("no break")
print("after", i)

j = 0
while j < 3:
    j += 1
else:
    print("while else", j)

print("==== strings ====")
print("a,b,c".split(","))
print("-".join(["x", "y", "z"]))
print("  pad  ".strip().upper())
print("%s=%d (%.2f)" % ("pi", 3, 3.14159))
print("{} {}".format(1, "a"))
print("abcdef"[1:4], "abcdef"[::-1])

print("==== exceptions ====")


def risky(v):
    try:
        if v < 0:
            raise ValueError("negative")
        return 100 / v
    except ZeroDivisionError:
        return "div0"
    except ValueError as e:
        return "value:" + str(e)
    else:
        return "ok"


print(risky(4), risky(0), risky(-1))

try:
    raise KeyError("missing")
except (ValueError, KeyError) as e:
    print("multi", e)
finally:
    print("cleanup")

print("==== files ====")
path = "py2c99_features_tmp.txt"
with open(path, "w") as f:
    f.write("one\ntwo\n")
with open(path) as f:
    data = f.read()
print(data.strip().split("\n"))
os.remove(path)
print("removed", os.path.exists(path))

print("==== dicts/sets ====")
d = {"a": 1, "b": 2}
d["c"] = 3
d.update({"d": 4})
print(d, d.get("z", 0), "a" in d, len(d))
s = set([1, 2, 2, 3])
s.add(4)
print(sorted(list(s)), 2 in s)
print(math.floor(3.7), math.ceil(3.2), round(2.5))
print("done")
