import tkinter as tk

root = tk.Tk()
root.title("smoke")
root.geometry("300x200")

label = tk.Label(root, text="start")
label.pack(pady=10)

count = tk.IntVar(value=0)


def clicked():
    count.set(count.get() + 1)
    print("click", count.get())
    label.config(text="clicked %d" % count.get())
    if count.get() >= 2:
        root.after(10, root.destroy)


button = tk.Button(root, text="go", command=clicked)
button.pack(pady=10)

root.after(50, button.invoke)
root.after(120, button.invoke)
root.mainloop()

print("smoke done, count =", count.get())
