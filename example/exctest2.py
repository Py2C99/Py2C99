print("before")
try:
    raise KeyError("missing")
except (ValueError, KeyError) as e:
    print("multi", e)
finally:
    print("cleanup")
print("after")
