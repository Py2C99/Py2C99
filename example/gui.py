import tkinter as tk

root = tk.Tk()
root.title("py2c99 demo")
root.geometry("380x260")

label = tk.Label(root, text="Hello from pure C99", fg="navy")
label.pack(pady=12)

entry = tk.Entry(root)
entry.pack()

count = tk.IntVar(value=0)


def clicked():
    count.set(count.get() + 1)
    label.config(text="Clicked %d time(s)" % count.get())
    entry.delete(0, tk.END)
    entry.insert(0, "count = %d" % count.get())


tk.Button(root, text="Click me", command=clicked).pack(pady=8)
tk.Button(root, text="Quit", command=root.destroy).pack(pady=4)

root.mainloop()
