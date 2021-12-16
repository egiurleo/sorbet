#include "packager/rbi_gen.h"
#include "absl/strings/match.h"
#include "absl/strings/str_replace.h"
#include "absl/synchronization/blocking_counter.h"
#include "common/FileOps.h"
#include "common/concurrency/ConcurrentQueue.h"
#include "common/concurrency/WorkerPool.h"
#include "core/GlobalState.h"
#include "packager/packager.h"

using namespace std;
namespace sorbet::packager {
namespace {

class Indent;

class Output final {
private:
    friend class Indent;

    fmt::memory_buffer out;
    int indent = 0;
    std::string tabStr = "";

    void resetTabString() {
        tabStr = string(indent * 2, ' ');
    }

    void tab() {
        indent++;
        resetTabString();
    }

    void untab() {
        indent--;
        resetTabString();
    }

public:
    template <typename... T> void println(fmt::format_string<T...> fmt, T &&...args) {
        fmt::format_to(std::back_inserter(out), tabStr);
        fmt::format_to(std::back_inserter(out), fmt, args...);
        fmt::format_to(std::back_inserter(out), "\n");
    }

    void println(string_view arg) {
        fmt::format_to(std::back_inserter(out), tabStr);
        // Hack: Intent even w/ multiline strings.
        string indented = absl::StrReplaceAll(arg, {{"\n", "\n" + tabStr}});
        std::copy(indented.begin(), indented.end(), std::back_inserter(out));
        fmt::format_to(std::back_inserter(out), "\n");
    }

    string toString() {
        auto output = fmt::to_string(out);
        out.clear();
        return output;
    }
};

class Indent {
private:
    Output &out;

public:
    Indent(Output &out) : out(out) {
        out.tab();
    }
    ~Indent() {
        out.untab();
    }
};

// TODO: copied from lsp_helpers.cc. Move to a common utils package.
// TODO: Respect indentation.
core::TypePtr getResultType(const core::GlobalState &gs, const core::TypePtr &type, core::SymbolRef inWhat,
                            core::TypePtr receiver, const core::TypeConstraint *constr) {
    auto resultType = type;
    if (core::is_proxy_type(receiver)) {
        receiver = receiver.underlying(gs);
    }
    if (core::isa_type<core::AppliedType>(receiver)) {
        auto &applied = core::cast_type_nonnull<core::AppliedType>(receiver);
        /* instantiate generic classes */
        resultType =
            core::Types::resultTypeAsSeenFrom(gs, resultType, inWhat.enclosingClass(gs), applied.klass, applied.targs);
    }
    if (!resultType) {
        resultType = core::Types::untypedUntracked();
    }
    if (receiver) {
        resultType = core::Types::replaceSelfType(gs, resultType, receiver); // instantiate self types
    }
    if (constr) {
        resultType = core::Types::instantiate(gs, resultType, *constr); // instantiate generic methods
    }
    return resultType;
}

// iff a sig has more than this many parameters, then print it as a multi-line sig.
constexpr int MAX_PRETTY_SIG_ARGS = 4;
// iff a `def` would be this wide or wider, expand it to be a multi-line def.
constexpr int MAX_PRETTY_WIDTH = 80;

core::SymbolRef lookupFQN(const core::GlobalState &gs, const vector<core::NameRef> &fqn) {
    core::SymbolRef scope = core::Symbols::root();
    for (auto name : fqn) {
        if (scope.isClassOrModule()) {
            auto result = scope.asClassOrModuleRef().data(gs)->findMember(gs, name);
            if (!result.exists()) {
                return core::Symbols::noClassOrModule();
            }
            scope = result;
        } else {
            return core::Symbols::noClassOrModule();
        }
    }
    return scope;
}

class RBIExporter final {
private:
    const core::GlobalState &gs;
    const core::packages::PackageInfo &pkg;
    const core::ClassOrModuleRef pkgNamespace;
    const core::ClassOrModuleRef pkgTestNamespace;
    const UnorderedSet<core::ClassOrModuleRef> &pkgNamespaces;
    UnorderedSet<core::SymbolRef> emittedSymbols;
    vector<core::SymbolRef> toEmit;
    void maybeEmit(core::SymbolRef symbol) {
        if (symbol.isClassOrModule() && symbol.asClassOrModuleRef().data(gs)->isSingletonClass(gs)) {
            maybeEmit(symbol.asClassOrModuleRef().data(gs)->attachedClass(gs));
            return;
        }
        if (!emittedSymbols.contains(symbol) && isInPackage(symbol)) {
            emittedSymbols.insert(symbol);
            toEmit.emplace_back(symbol);
        }
    }

    string prettySigForMethod(const core::GlobalState &gs, core::MethodRef method, const core::TypePtr &receiver,
                              core::TypePtr retType, const core::TypeConstraint *constraint) {
        ENFORCE(method.exists());
        ENFORCE(method.data(gs)->dealiasMethod(gs) == method);
        // handle this case anyways so that we don't crash in prod when this method is mis-used
        if (!method.exists()) {
            return "";
        }

        if (!retType) {
            retType = getResultType(gs, method.data(gs)->resultType, method, receiver, constraint);
        }
        if (retType) {
            enqueueSymbolsInType(retType);
        }
        string methodReturnType =
            (retType == core::Types::void_()) ? "void" : absl::StrCat("returns(", retType.show(gs), ")");
        vector<string> typeAndArgNames;
        vector<string> typeArguments;

        vector<string> flags;
        auto sym = method.data(gs);
        string sigCall = "sig";
        if (sym->flags.isFinal) {
            sigCall = "sig(:final)";
        }
        if (sym->flags.isAbstract) {
            flags.emplace_back("abstract");
        }
        if (sym->flags.isOverridable) {
            flags.emplace_back("overridable");
        }
        if (sym->flags.isOverride) {
            flags.emplace_back("override");
        }
        for (auto ta : method.data(gs)->typeArguments) {
            typeArguments.emplace_back(absl::StrCat(":", ta.data(gs)->name.show(gs)));
        }
        for (auto &argSym : method.data(gs)->arguments) {
            // Don't display synthetic arguments (like blk).
            if (!argSym.isSyntheticBlockArgument()) {
                auto argType = getResultType(gs, argSym.type, method, receiver, constraint);
                enqueueSymbolsInType(argType);
                typeAndArgNames.emplace_back(absl::StrCat(argSym.argumentName(gs), ": ", argType.show(gs)));
            }
        }

        string flagString = "";
        if (!flags.empty()) {
            flagString = fmt::format("{}.", fmt::join(flags, "."));
        }
        string typeParamsString = "";
        if (!typeArguments.empty()) {
            typeParamsString = fmt::format("type_parameters({}).", fmt::join(typeArguments, ", "));
        }
        string paramsString = "";
        if (!typeAndArgNames.empty()) {
            paramsString = fmt::format("params({}).", fmt::join(typeAndArgNames, ", "));
        }

        auto oneline =
            fmt::format("{} {{{}{}{}{}}}", sigCall, flagString, typeParamsString, paramsString, methodReturnType);
        if (oneline.size() <= MAX_PRETTY_WIDTH && typeAndArgNames.size() <= MAX_PRETTY_SIG_ARGS) {
            return oneline;
        }

        if (!flags.empty()) {
            flagString = fmt::format("{}\n  .", fmt::join(flags, "\n  ."));
        }
        if (!typeArguments.empty()) {
            typeParamsString = fmt::format("type_parameters({})\n  .", fmt::join(typeArguments, ", "));
        }
        if (!typeAndArgNames.empty()) {
            paramsString = fmt::format("params(\n    {}\n  )\n  .", fmt::join(typeAndArgNames, ",\n    "));
        }
        return fmt::format("{} do\n  {}{}{}{}\nend", sigCall, flagString, typeParamsString, paramsString,
                           methodReturnType);
    }

    string prettyDefForMethod(const core::GlobalState &gs, core::MethodRef method) {
        ENFORCE(method.exists());
        // handle this case anyways so that we don't crash in prod when this method is mis-used
        if (!method.exists()) {
            return "";
        }
        auto methodData = method.data(gs);

        string visibility = "";
        if (methodData->flags.isPrivate) {
            visibility = "private ";
        } else if (methodData->flags.isProtected) {
            visibility = "protected ";
        }

        auto methodNameRef = methodData->name;
        ENFORCE(methodNameRef.exists());
        string methodName = "???";
        if (methodNameRef.exists()) {
            methodName = methodNameRef.toString(gs);
        }
        string methodNamePrefix = "";
        if (methodData->owner.exists() && methodData->owner.data(gs)->attachedClass(gs).exists()) {
            methodNamePrefix = "self.";
        }
        vector<string> prettyArgs;
        const auto &arguments = methodData->dealiasMethod(gs).data(gs)->arguments;
        ENFORCE(!arguments.empty(), "Should have at least a block arg");
        for (const auto &argSym : arguments) {
            // Don't display synthetic arguments (like blk).
            if (argSym.isSyntheticBlockArgument()) {
                continue;
            }
            string prefix = "";
            string suffix = "";
            if (argSym.flags.isRepeated) {
                if (argSym.flags.isKeyword) {
                    prefix = "**"; // variadic keyword args
                } else {
                    prefix = "*"; // rest args
                }
            } else if (argSym.flags.isKeyword) {
                if (argSym.flags.isDefault) {
                    suffix = ": T.let(T.unsafe(nil), T.untyped)"; // optional keyword (has a default value)
                } else {
                    suffix = ":"; // required keyword
                }
            } else if (argSym.flags.isBlock) {
                prefix = "&";
            } else if (argSym.flags.isDefault) {
                suffix = "= T.let(T.unsafe(nil), T.untyped)";
            }
            prettyArgs.emplace_back(fmt::format("{}{}{}", prefix, argSym.argumentName(gs), suffix));
        }

        string argListPrefix = "";
        string argListSeparator = "";
        string argListSuffix = "";
        if (prettyArgs.size() > 0) {
            argListPrefix = "(";
            argListSeparator = ", ";
            argListSuffix = ")";
        }

        auto result = fmt::format("{}def {}{}{}{}{}", visibility, methodNamePrefix, methodName, argListPrefix,
                                  fmt::join(prettyArgs, argListSeparator), argListSuffix);
        if (prettyArgs.size() > 0 && result.length() >= MAX_PRETTY_WIDTH) {
            argListPrefix = "(\n  ";
            argListSeparator = ",\n  ";
            argListSuffix = "\n)";
            result = fmt::format("{}def {}{}{}{}{}", visibility, methodNamePrefix, methodName, argListPrefix,
                                 fmt::join(prettyArgs, argListSeparator), argListSuffix);
        }
        return result;
    }

    void enqueueSymbolsInType(const core::TypePtr &type) {
        switch (type.tag()) {
            case core::TypePtr::Tag::AliasType: {
                const auto &alias = core::cast_type_nonnull<core::AliasType>(type);
                maybeEmit(alias.symbol);
                break;
            }
            case core::TypePtr::Tag::AndType: {
                const auto &andType = core::cast_type_nonnull<core::AndType>(type);
                enqueueSymbolsInType(andType.left);
                enqueueSymbolsInType(andType.right);
                break;
            }
            case core::TypePtr::Tag::AppliedType: {
                const auto &applied = core::cast_type_nonnull<core::AppliedType>(type);
                maybeEmit(applied.klass);
                for (auto &targ : applied.targs) {
                    enqueueSymbolsInType(targ);
                }
                break;
            }
            case core::TypePtr::Tag::BlamedUntyped: {
                break;
            }
            case core::TypePtr::Tag::ClassType: {
                const auto &classType = core::cast_type_nonnull<core::ClassType>(type);
                maybeEmit(classType.symbol);
                break;
            }
            case core::TypePtr::Tag::LiteralType: {
                // No symbols here.
                break;
            }
            case core::TypePtr::Tag::MetaType: {
                const auto &metaType = core::cast_type_nonnull<core::MetaType>(type);
                enqueueSymbolsInType(metaType.wrapped);
                break;
            }
            case core::TypePtr::Tag::OrType: {
                const auto &orType = core::cast_type_nonnull<core::OrType>(type);
                enqueueSymbolsInType(orType.left);
                enqueueSymbolsInType(orType.right);
                break;
            }
            case core::TypePtr::Tag::SelfType: {
                break;
            }
            case core::TypePtr::Tag::SelfTypeParam: {
                const auto &selfTypeParam = core::cast_type_nonnull<core::SelfTypeParam>(type);
                maybeEmit(selfTypeParam.definition);
                break;
            }
            case core::TypePtr::Tag::ShapeType: {
                const auto &shapeType = core::cast_type_nonnull<core::ShapeType>(type);
                for (const auto &key : shapeType.keys) {
                    enqueueSymbolsInType(key);
                }
                for (const auto &value : shapeType.values) {
                    enqueueSymbolsInType(value);
                }
                break;
            }
            case core::TypePtr::Tag::TupleType: {
                const auto &tupleType = core::cast_type_nonnull<core::TupleType>(type);
                for (const auto &elem : tupleType.elems) {
                    enqueueSymbolsInType(elem);
                }
                break;
            }
            case core::TypePtr::Tag::TypeVar: {
                break;
            }
            case core::TypePtr::Tag::UnresolvedAppliedType: {
                const auto &unresolvedAppliedType = core::cast_type_nonnull<core::UnresolvedAppliedType>(type);
                maybeEmit(unresolvedAppliedType.klass);
                maybeEmit(unresolvedAppliedType.symbol);
                for (const auto &targ : unresolvedAppliedType.targs) {
                    enqueueSymbolsInType(targ);
                }
                break;
            }
            case core::TypePtr::Tag::UnresolvedClassType: {
                break;
            }
            case core::TypePtr::Tag::LambdaParam: {
                const auto &lambdaParam = core::cast_type_nonnull<core::LambdaParam>(type);
                enqueueSymbolsInType(lambdaParam.lowerBound);
                enqueueSymbolsInType(lambdaParam.upperBound);
                break;
            }
        }
    }

    Output out;

    // copied from variance.cc
    string showVariance(core::TypeMemberRef tm) {
        if (tm.data(gs)->isFixed()) {
            auto &lambdaParam = core::cast_type_nonnull<core::LambdaParam>(tm.data(gs)->resultType);
            return absl::StrCat("fixed: ", lambdaParam.upperBound.toString(gs));
        }

        switch (tm.data(gs)->variance()) {
            case core::Variance::CoVariant:
                return ":out";
            case core::Variance::Invariant:
                return ":invariant";
            case core::Variance::ContraVariant:
                return ":in";
        }
    }

    bool isInTestPackage(core::SymbolRef klass) {
        if (klass == core::Symbols::root() || klass == core::Symbols::PackageRegistry()) {
            return false;
        }
        if (klass == pkgNamespace) {
            return false;
        }
        if (klass == pkgTestNamespace) {
            return true;
        }
        if (klass.isClassOrModule()) {
            if (pkgNamespaces.contains(klass.asClassOrModuleRef())) {
                return false;
            }
        }
        return isInTestPackage(klass.owner(gs));
    }

    bool isInPackage(core::SymbolRef klass) {
        if (klass == core::Symbols::root() || klass == core::Symbols::PackageRegistry()) {
            return false;
        }
        if (klass == pkgNamespace || klass == pkgTestNamespace) {
            return true;
        }
        if (klass.isClassOrModule()) {
            if (pkgNamespaces.contains(klass.asClassOrModuleRef())) {
                return false;
            }
        }
        return isInPackage(klass.owner(gs));
    }

    string typeDeclaration(const core::TypePtr &type) {
        if (type == nullptr) {
            return absl::StrCat("T.let(T.unsafe(nil), ", core::Types::untypedUntracked().show(gs), ")");
        } else if (core::isa_type<core::AliasType>(type)) {
            auto alias = core::cast_type_nonnull<core::AliasType>(type);
            return alias.symbol.show(gs);
        } else {
            return absl::StrCat("T.let(T.unsafe(nil), ", type.show(gs), ")");
        }
    }

    bool shouldSkipMember(core::NameRef name) {
        if (name.kind() == core::NameKind::UNIQUE) {
            return true;
        }

        return name == core::Names::singleton() || name == core::Names::Constants::AttachedClass() ||
               name == core::Names::attached();
    }

    void emit(core::ClassOrModuleRef klass) {
        if (!isInPackage(klass) || !emittedSymbols.contains(klass)) {
            // We don't emit class definitions for items defined in other packages.
            Exception::raise("Invalid klass");
        }

        if (klass.data(gs)->superClass().data(gs)->superClass() == core::Symbols::T_Enum()) {
            // Enum value class created in TEnum rewriter pass.
            return;
        }

        const bool isEnum = klass.data(gs)->superClass() == core::Symbols::T_Enum();

        // cerr << "Emitting " << klass.show(gs) << "\n";
        // Class definition line
        auto defType = klass.data(gs)->isClassOrModuleClass() ? "class" : "module";
        auto fullName = klass.show(gs);
        string superClassString;
        if (klass.data(gs)->superClass().exists()) {
            auto superClass = klass.data(gs)->superClass();
            if (superClass != core::Symbols::Sorbet_Private_Static_ImplicitModuleSuperClass()) {
                maybeEmit(superClass);
                superClassString = absl::StrCat(" < ", superClass.show(gs));
            }
        }
        out.println("{} {}{}", defType, fullName, superClassString);

        {
            Indent indent(out);

            if (klass.data(gs)->isClassOrModuleAbstract()) {
                out.println("abstract!");
            }

            if (klass.data(gs)->isClassOrModuleFinal()) {
                out.println("final!");
            }

            if (klass.data(gs)->isClassOrModuleInterface()) {
                out.println("interface!");
            }

            if (klass.data(gs)->isClassOrModuleSealed()) {
                out.println("sealed!");
            }

            // Mixins (include/extend)
            for (auto mixin : klass.data(gs)->mixins()) {
                auto isSingleton = mixin.data(gs)->isSingletonClass(gs);
                auto keyword = isSingleton ? "extend"sv : "include"sv;
                out.println("{} {}", keyword, mixin.show(gs));
                maybeEmit(mixin);
            }

            // Type members
            for (auto typeMember : klass.data(gs)->typeMembers()) {
                emit(typeMember);
            }

            // Members
            core::MethodRef initializeMethod;
            vector<core::FieldRef> pendingFields;
            vector<core::ClassOrModuleRef> pendingEnumValues;
            for (auto &[name, member] : klass.data(gs)->membersStableOrderSlow(gs)) {
                if (shouldSkipMember(name)) {
                    continue;
                }

                switch (member.kind()) {
                    case core::SymbolRef::Kind::ClassOrModule: {
                        auto memberKlass = member.asClassOrModuleRef();
                        if (isEnum && memberKlass.data(gs)->superClass() == klass) {
                            pendingEnumValues.emplace_back(memberKlass);
                        } else {
                            // Emit later.
                            maybeEmit(member);
                        }
                        break;
                    }
                    case core::SymbolRef::Kind::TypeMember: {
                        // Ignore; already emitted above.
                        break;
                    }
                    case core::SymbolRef::Kind::TypeArgument: {
                        ENFORCE(false, "classes should never contain type arguments");
                        break;
                    }
                    case core::SymbolRef::Kind::Method: {
                        if (name == core::Names::initialize()) {
                            // Defer outputting until we gather fields.
                            initializeMethod = member.asMethodRef();
                        } else {
                            emit(member.asMethodRef());
                        }
                        break;
                    }
                    case core::SymbolRef::Kind::FieldOrStaticField: {
                        auto field = member.asFieldRef();
                        if (field.data(gs)->isField()) {
                            pendingFields.emplace_back(field);
                        } else {
                            if (absl::StartsWith(field.data(gs)->name.show(gs), "@@")) {
                                emit(field, true);
                            } else {
                                maybeEmit(field);
                            }
                        }
                        break;
                    }
                }
            }

            maybeEmitInitialized(initializeMethod, pendingFields);
            pendingFields.clear();

            auto singleton = klass.data(gs)->lookupSingletonClass(gs);
            if (singleton.exists()) {
                // Mixins (include/extend)
                for (auto mixin : singleton.data(gs)->mixins()) {
                    out.println("extend {}", mixin.show(gs));
                    maybeEmit(mixin);
                }

                // Type templates
                for (auto typeMember : singleton.data(gs)->typeMembers()) {
                    emit(typeMember);
                }

                for (auto &[name, member] : singleton.data(gs)->membersStableOrderSlow(gs)) {
                    if (shouldSkipMember(name)) {
                        continue;
                    }

                    switch (member.kind()) {
                        case core::SymbolRef::Kind::ClassOrModule: {
                            maybeEmit(member);
                            break;
                        }
                        case core::SymbolRef::Kind::TypeMember: {
                            // Ignore; already emitted above.
                            break;
                        }
                        case core::SymbolRef::Kind::TypeArgument: {
                            ENFORCE(false, "classes should never contain type arguments");
                            break;
                        }
                        case core::SymbolRef::Kind::Method: {
                            if (isEnum && name == core::Names::sealedSubclasses()) {
                                // Ignore: Generated by TEnum rewriter pass.
                            } else {
                                emit(member.asMethodRef());
                            }
                            break;
                        }
                        case core::SymbolRef::Kind::FieldOrStaticField: {
                            auto field = member.asFieldRef();
                            if (field.data(gs)->isField()) {
                                emit(field);
                            } else {
                                if (absl::StartsWith(field.data(gs)->name.show(gs), "@@")) {
                                    emit(field, true);
                                } else {
                                    maybeEmit(field);
                                }
                            }
                            break;
                        }
                    }
                }
            }

            if (isEnum && !pendingEnumValues.empty()) {
                out.println("enums do");
                {
                    Indent indentEnumBlock(out);
                    for (auto enumVal : pendingEnumValues) {
                        out.println("{} = new", enumVal.data(gs)->name.show(gs));
                    }
                }
                out.println("end");
            }
        }

        out.println("end");
    }

    void emit(core::MethodRef method) {
        if (emittedSymbols.contains(method)) {
            return;
        }

        if (method.data(gs)->name == core::Names::staticInit()) {
            return;
        }
        emittedSymbols.insert(method);

        if (method.data(gs)->flags.isPrivate) {
            // Private methods will never be called outside of a package.
            return;
        }

        // cerr << "Emitting " << method.show(gs) << "\n";

        for (auto &arg : method.data(gs)->arguments) {
            enqueueSymbolsInType(arg.type);
        }

        if (method.data(gs)->hasSig()) {
            out.println(prettySigForMethod(gs, method, nullptr, method.data(gs)->resultType, nullptr));
        }
        out.println(prettyDefForMethod(gs, method) + "; end");
    }

    void maybeEmitInitialized(core::MethodRef method, const std::vector<core::FieldRef> &fields) {
        if (fields.empty() && !method.exists()) {
            return;
        }
        // cerr << "Emitting initialized\n";
        string methodDef;
        if (method.exists()) {
            if (method.data(gs)->owner.data(gs)->superClass() == core::Symbols::T_Struct()) {
                // Don't emit `initialize` methods for `T::Struct`s. Rewriter will create them.
                return;
            }
            if (method.data(gs)->hasSig()) {
                out.println(prettySigForMethod(gs, method, nullptr, method.data(gs)->resultType, nullptr));
            }
            methodDef = prettyDefForMethod(gs, method);
        } else {
            out.println("sig {void}");
            methodDef = "def initialize";
        }

        if (fields.empty()) {
            out.println(methodDef + "; end");
        } else {
            out.println(methodDef);
            {
                Indent indent(out);
                for (auto field : fields) {
                    emit(field);
                }
            }
            out.println("end");
        }
    }
    void emit(core::FieldRef field, bool isCVar = false) {
        // cerr << "Emitting " << field.show(gs) << "\n";
        if (field.data(gs)->isStaticField()) {
            const auto &resultType = field.data(gs)->resultType;
            if (resultType != nullptr) {
                if (core::isa_type<core::AliasType>(resultType)) {
                    const auto &alias = core::cast_type_nonnull<core::AliasType>(resultType);
                    if (alias.symbol.isTypeMember() &&
                        alias.symbol.asTypeMemberRef().data(gs)->owner.asClassOrModuleRef().data(gs)->isSingletonClass(
                            gs)) {
                        // type_templates define static fields of the same name on the main class; ignore them.
                        return;
                    }
                } else if (core::isa_type<core::ClassType>(resultType)) {
                    auto klass = core::cast_type_nonnull<core::ClassType>(resultType).symbol;
                    if (klass.data(gs)->superClass().data(gs)->superClass() == core::Symbols::T_Enum()) {
                        // Static field defined in TEnum rewriter pass.
                        return;
                    }
                }
            }

            if (isCVar) {
                out.println("{} = {}", field.data(gs)->name.show(gs), typeDeclaration(resultType));
            } else {
                out.println("{} = {}", field.show(gs), typeDeclaration(resultType));
            }
        } else {
            out.println("{} = {}", field.data(gs)->name.show(gs), typeDeclaration(field.data(gs)->resultType));
        }
    }

    void emit(core::TypeMemberRef tm) {
        if (emittedSymbols.contains(tm)) {
            return;
        }
        emittedSymbols.insert(tm);

        // cerr << "Emitting " << tm.show(gs) << "\n";

        if (tm.data(gs)->name == core::Names::Constants::AttachedClass()) {
            return;
        }

        // If this is a type template, there will be an alias type defined on the non-singleton class w/ the same name.
        if (tm.data(gs)->owner.asClassOrModuleRef().data(gs)->isSingletonClass(gs)) {
            out.println("{} = type_template({})", tm.data(gs)->name.show(gs), showVariance(tm));
        } else {
            out.println("{} = type_member({})", tm.data(gs)->name.show(gs), showVariance(tm));
        }
    }

    void emitLoop() {
        while (!toEmit.empty()) {
            auto symbol = toEmit.back();
            toEmit.pop_back();
            switch (symbol.kind()) {
                case core::SymbolRef::Kind::ClassOrModule:
                    emit(symbol.asClassOrModuleRef());
                    break;
                case core::SymbolRef::Kind::Method:
                    emit(symbol.asMethodRef());
                    break;
                case core::SymbolRef::Kind::FieldOrStaticField:
                    emit(symbol.asFieldRef());
                    break;
                case core::SymbolRef::Kind::TypeMember:
                    break;
                case core::SymbolRef::Kind::TypeArgument:
                    break;
            }
        }
    }

    static core::ClassOrModuleRef getPkgTestNamespace(const core::GlobalState &gs,
                                                      const core::packages::PackageInfo &pkg) {
        vector<core::NameRef> fullName = pkg.fullName();
        fullName.insert(fullName.begin(), core::Names::Constants::Test());
        return lookupFQN(gs, fullName).asClassOrModuleRef();
    }

public:
    RBIExporter(const core::GlobalState &gs, const core::packages::PackageInfo &pkg,
                const UnorderedSet<core::ClassOrModuleRef> &pkgNamespaces)
        : gs(gs), pkg(pkg), pkgNamespace(lookupFQN(gs, pkg.fullName()).asClassOrModuleRef()),
          pkgTestNamespace(getPkgTestNamespace(gs, pkg)), pkgNamespaces(pkgNamespaces) {}

    RBIGenerator::RBIOutput emit() {
        RBIGenerator::RBIOutput output;
        output.baseFilePath = pkg.mangledName().show(gs);

        auto rawExports = pkg.exports();
        auto rawTestExports = pkg.testExports();

        vector<core::SymbolRef> exports;
        vector<core::SymbolRef> testExports;

        for (auto &e : rawExports) {
            auto exportSymbol = lookupFQN(gs, e);
            if (exportSymbol.exists()) {
                if (isInTestPackage(exportSymbol)) {
                    // Test:: symbol.
                    testExports.emplace_back(exportSymbol);
                } else {
                    exports.emplace_back(exportSymbol);
                }
            }
        }

        for (auto e : rawTestExports) {
            auto exportSymbol = lookupFQN(gs, e);
            if (exportSymbol.exists()) {
                testExports.emplace_back(exportSymbol);
            }
        }

        if (!exports.empty()) {
            for (auto &exportSymbol : exports) {
                maybeEmit(exportSymbol);
            }

            emitLoop();

            output.rbi = "# typed: true\n\n" + out.toString();
        }

        if (!testExports.empty()) {
            for (auto exportSymbol : testExports) {
                maybeEmit(exportSymbol);
            }

            emitLoop();

            auto rbiText = out.toString();
            if (!rbiText.empty()) {
                output.testRBI = "# typed: true\n\n" + rbiText;
            }
        }

        return output;
    }
};
} // namespace

UnorderedSet<core::ClassOrModuleRef>
RBIGenerator::buildPackageNamespace(core::GlobalState &gs, vector<ast::ParsedFile> &packageFiles, WorkerPool &workers) {
    // Populate package database.
    packageFiles = Packager::findPackages(gs, workers, move(packageFiles));

    const auto &packageDB = gs.packageDB();

    auto &packages = packageDB.packages();
    if (packages.empty()) {
        Exception::raise("No packages found?");
    }

    auto testNamespace = core::Names::Constants::Test();

    UnorderedSet<core::ClassOrModuleRef> packageNamespaces;
    for (auto package : packages) {
        auto &pkg = gs.packageDB().getPackageInfo(package);
        vector<core::NameRef> fullName = pkg.fullName();
        auto packageNamespace = lookupFQN(gs, fullName);
        // Might not exist if package has no files.
        if (packageNamespace.exists()) {
            packageNamespaces.insert(packageNamespace.asClassOrModuleRef());
        }

        fullName.insert(fullName.begin(), testNamespace);
        auto testPackageNamespace = lookupFQN(gs, fullName);
        if (testPackageNamespace.exists()) {
            packageNamespaces.insert(testPackageNamespace.asClassOrModuleRef());
        }
    }

    return packageNamespaces;
}

RBIGenerator::RBIOutput RBIGenerator::runOnce(const core::GlobalState &gs, core::NameRef pkgName,
                                              const UnorderedSet<core::ClassOrModuleRef> &packageNamespaces) {
    auto &pkg = gs.packageDB().getPackageInfo(pkgName);
    ENFORCE(pkg.exists());
    RBIExporter exporter(gs, pkg, packageNamespaces);
    return exporter.emit();
}

void RBIGenerator::run(core::GlobalState &gs, vector<ast::ParsedFile> packageFiles, string outputDir,
                       WorkerPool &workers) {
    absl::BlockingCounter threadBarrier(std::max(workers.size(), 1));
    UnorderedSet<core::ClassOrModuleRef> packageNamespaces = buildPackageNamespace(gs, packageFiles, workers);

    const auto &packageDB = gs.packageDB();
    auto &packages = packageDB.packages();
    auto inputq = make_shared<ConcurrentBoundedQueue<core::NameRef>>(packages.size());
    for (auto package : packages) {
        inputq->push(move(package), 1);
    }

    const core::GlobalState &rogs = gs;
    workers.multiplexJob("RBIGenerator", [inputq, outputDir, &threadBarrier, &rogs, &packageNamespaces]() {
        core::NameRef job;
        for (auto result = inputq->try_pop(job); !result.done(); result = inputq->try_pop(job)) {
            if (result.gotItem()) {
                auto output = runOnce(rogs, job, packageNamespaces);
                if (!output.rbi.empty()) {
                    FileOps::write(absl::StrCat(outputDir, "/", output.baseFilePath, ".rbi"), output.rbi);
                }

                if (!output.testRBI.empty()) {
                    FileOps::write(absl::StrCat(outputDir, "/", output.baseFilePath, ".test.rbi"), output.testRBI);
                }
            }
        }
        threadBarrier.DecrementCount();
    });
    threadBarrier.Wait();
}
} // namespace sorbet::packager