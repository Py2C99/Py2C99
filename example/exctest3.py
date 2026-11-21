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


print("a", risky(4), risky(0), risky(-1))

try:
    raise KeyError("missing")
except (ValueError, KeyError) as e:
    print("multi", e)
finally:
    print("cleanup")
print("after")
