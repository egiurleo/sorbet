# typed: true

def bar(a, b, c); end
def baz(a:, b:, c:); end

def foo(*)
  bar(*)
end

def buzz(**)
  baz(**)
end
