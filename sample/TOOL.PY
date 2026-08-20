# the demo workspace's Python file
def fib(n):
    """First n Fibonacci numbers."""
    a, b, out = 0, 1, []
    for _ in range(n):
        out.append(a)
        a, b = b, a + b
    return out


if __name__ == "__main__":
    print(fib(10))
