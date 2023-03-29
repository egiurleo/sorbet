# typed: true

def bar(a, b, c); end
def baz(a:, b:, c:); end
def buzz(a, b, c:, &blk); end

def foo(*)
  bar(*)
end

# def buzz(**)
#   baz(**)
# end
