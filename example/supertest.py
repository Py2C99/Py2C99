class A:
    def speak(self):
        return "A"


class B(A):
    def speak(self):
        return super().speak() + "B"


class C(B):
    def speak(self):
        return super().speak() + "C"


print(B().speak())
print(C().speak())
print("done")
