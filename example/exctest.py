print("before")
try:
    raise KeyError("missing")
except KeyError as e:
    print("caught", e)
print("after")
