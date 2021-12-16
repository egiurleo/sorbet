# typed: strict

module RBIGen::Private
  class PrivateClass; end

  class PrivateClassNotReferenced; end

  class PrivateClassForTests; end
end

module RBIGen::Public
  class RefersToPrivateTypes
    extend T::Sig

    sig {params(a: RBIGen::Private::PrivateClass).void}
    def method(a)
    end
  end

  class FinalClass
    extend T::Helpers
    extend T::Sig
    final!

    sig(:final) {void}
    def final_method; end
  end

  class SealedClass
    extend T::Helpers
    sealed!
  end

  class AbstractClass
    extend T::Helpers
    extend T::Sig
    abstract!

    sig {abstract.returns(String)}
    def abstract_method; end
  end

  module InterfaceModule
    extend T::Helpers
    extend T::Sig
    interface!

    sig {abstract.returns(String)}
    def interface_method; end
  end

  class AbstractAndInterfaceImplementor < AbstractClass
    include InterfaceModule

    sig {override.returns(String)}
    def abstract_method
      ""
    end

    sig {override.returns(String)}
    def interface_method
      ""
    end
  end

  class MyEnum < T::Enum
    extend T::Sig
    
    enums do
      Spades = new
      Hearts = new
      Clubs = new
      Diamonds = new
    end

    sig {returns(String)}
    def to_string
      ""
    end
  end

  class MyStruct < T::Struct
    extend T::Sig
    
    prop :foo, Integer
    const :bar, T.nilable(String)
    const :quz, Float, default: 0.5

    sig {returns(Integer)}
    def extra_method
      10
    end
  end

  class FieldCheck
    extend T::Sig
  
    Alias = RBIGen::Public::FieldCheck
    Constant = T.let(0, Integer)
    AliasConstant = T.let(RBIGen::Public::FieldCheck, T.class_of(RBIGen::Public::FieldCheck))

    @@static_field = T.let(10, Integer)

    @statically_declared_field = T.let(0, Integer)

    sig {void}
    def initialize
      @field = T.let(0, Integer)
    end
  end
end