s = "  Hello, World  "
print(s.strip())
print(s.lstrip())
print(s.rstrip())
print(s.upper(), s.lower())
print("abc".startswith("ab"), "abc".endswith("bc"))
print("banana".find("an"), "banana".rfind("an"), "banana".count("an"))
print("banana".replace("a", "o", 2))
print("a-b-c".split("-"))
print("a b  c".split())
print("ab".center(6, "-"), "ab".ljust(4, "."), "ab".rjust(4, "."))
print("42".zfill(5))
print("abc".isalpha(), "123".isdigit(), "  ".isspace())
print("A".isupper(), "a".islower())
print("one\ntwo\n".splitlines())
print("k=v=w".partition("="))

l = [3, 1, 2]
l.append(4)
l.extend([5, 6])
l.insert(0, 0)
print(l)
print(l.pop(), l)
l.remove(3)
print(l)
print(l.index(2), l.count(2))
l.sort()
print(l)
l.reverse()
print(l)
print(l.copy(), len(l))
l.clear()
print(l)

d = {"a": 1, "b": 2}
print(sorted(d.keys()), sorted(d.values()), d.items())
print(d.get("a"), d.get("z", 9))
d["c"] = 3
d.update({"d": 4})
print(d.pop("d"), "d" in d)
print(d.setdefault("e", 5), d["e"])
print(dict(a=1, b=2))

st = {1, 2, 3}
st.add(4)
st.discard(2)
print(sorted(st))
print(sorted({1, 2, 3}.union({3, 4})))
print(sorted({1, 2, 3}.intersection({2, 3, 4})))
print(sorted({1, 2, 3}.difference({2})))
print({1, 2}.issubset({1, 2, 3}), {1, 2, 3}.issuperset({1}))

print(abs(-5), min(3, 1, 2), max([4, 9, 2]), sum([1, 2, 3]))
print(list(reversed([1, 2, 3])))
print(list(map(lambda x: x * 2, [1, 2, 3])))
print(list(filter(lambda x: x % 2, [1, 2, 3, 4])))
print(chr(65), ord("A"), round(2.6), pow(2, 10), divmod(7, 3))
print(hex(255), oct(8), bin(5), repr("x"))
print(format(3.14159, ".2f"), format(42, "05d"))
print(isinstance(1, int), isinstance("x", str), isinstance(1.0, (int, float)))
print(type(1), type("x"), type([]), type({}))

x = 7
print("big" if x > 5 else "small")
print(0 < x < 10, x == 7 and not False)
print(f"{x:04d} {3.14159:.2f} {'s':>4}")

def kw(a, b=2, *rest, c=3, **opts):
    return (a, b, rest, c, sorted(opts.items()))

print(kw(1))
print(kw(1, 5, 6, 7, c=8, d=9))
print(kw(a=1, c=4))

def outer2():
    total = 0
    add = lambda v: total + v
    total = 10
    return add(5)

print(outer2())

data = [1, 2, 3, 4, 5]
print([v * 2 for v in data if v % 2 == 1])
print({v: v * v for v in data})
print({v % 3 for v in data})
print(list(v + 1 for v in data if v < 3))
print(dict((k, len(k)) for k in ["a", "bb", "ccc"]))

n = 0
while n < 3:
    n += 1
print(n)
print("done")
