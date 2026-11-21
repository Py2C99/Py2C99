import tkinter as tk

root = tk.Tk()
root.geometry("260x120")
count = tk.IntVar(value=0)


def clicked():
    count.set(count.get() + 1)
    print("click", count.get())


button = tk.Button(root, text="go", command=clicked)
button.pack(pady=10)

root.after(50, button.invoke)
root.after(800, root.destroy)
root.mainloop()
print("done", count.get())
