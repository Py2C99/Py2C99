import math
import cmath
import operator
import collections
import heapq
import bisect
import json
import re
import textwrap
import html
import unicodedata
import fnmatch
import pathlib
import calendar
from urllib.parse import quote, unquote, parse_qs
from functools import reduce
from itertools import chain

print(math.sqrt(9), math.factorial(5), round(math.pi, 3))
print(cmath.phase(-1))
print(operator.add(2, 3), operator.lt(2, 3), operator.contains([1, 2], 2))
print(dict(collections.Counter("abca")))

h = [3, 1, 2]
heapq.heapify(h)
print(h, heapq.heappop(h))

xs = [1, 2, 3]
print(bisect.bisect_left(xs, 2), bisect.bisect_right(xs, 2))
bisect.insort(xs, 2)
print(xs)

print(json.dumps({"b": 2, "a": [1, True, None]}))
print(json.loads('{"x": [1, 2]}'))
print(re.findall(r"\d+", "a1b22"), bool(re.search(r"^a.*c$", "abc")))
print(textwrap.fill("one two three four", width=7))
print(html.escape("<a>&'\""))
print(unicodedata.category("A"), unicodedata.digit("7"))
print(fnmatch.fnmatch("test.txt", "*.txt"))
print(calendar.isleap(2024), calendar.isleap(2023), calendar.weekday(2024, 1, 1))
print(quote("a b/c"), unquote("a%20b"), parse_qs("a=1&a=2&b=3"))
print(reduce(lambda a, b: a + b, [1, 2, 3]))
print(list(chain([1, 2], [3])))
