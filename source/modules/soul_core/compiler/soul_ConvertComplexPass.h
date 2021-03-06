/*
    _____ _____ _____ __
   |   __|     |  |  |  |      The SOUL language
   |__   |  |  |  |  |  |__    Copyright (c) 2019 - ROLI Ltd.
   |_____|_____|_____|_____|

   The code in this file is provided under the terms of the ISC license:

   Permission to use, copy, modify, and/or distribute this software for any purpose
   with or without fee is hereby granted, provided that the above copyright notice and
   this permission notice appear in all copies.

   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
   TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
   NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
   DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
   IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
   CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

namespace soul
{

//==============================================================================
/**
    Converts complex primitives into an implementation using the soul::complex_lib namespace
    which contains a struct based complex number implementation.

    The transformation is performed in multiple passes. Binary and Unary operators are replaced by calls to
    associated methods in the namespace, and complex member references (e.g. real and imag) and mapped to the
    appropriate struct members. Finally instantiations of the complex_lib namespace are added for the given data
    type and vector size, and the complex types are replaced with the appropriate ComplexType structs

    A subsequent resolution pass is required to resolve the identifiers added by this process.
*/
struct ConvertComplexPass  final
{
    static void run (AST::Allocator& a, AST::ModuleBase& m)
    {
        ConvertComplexPass (a, m).run();
    }

private:
    ConvertComplexPass (AST::Allocator& a, AST::ModuleBase& m) : allocator (a), module (m)
    {
        intrinsicsNamespacePath = IdentifierPath::fromString (allocator.identifiers, getIntrinsicsNamespaceName());
    }

    AST::Allocator& allocator;
    AST::ModuleBase& module;
    IdentifierPath intrinsicsNamespacePath;

    static AST::QualifiedIdentifier& identifierFromString (AST::Allocator& allocator, AST::Context& context, const std::string& s)
    {
        return allocator.allocate<AST::QualifiedIdentifier> (context,  soul::IdentifierPath::fromString (allocator.identifiers, s));
    }

    void run()
    {
        ConvertComplexOperators (*this).visitObject (module);
        ConvertComplexElementAccess (*this).visitObject (module);
        ConvertComplexRemapTypes (*this).visitObject (module);
    }

    struct ComplexBase : public RewritingASTVisitor
    {
        ComplexBase (ConvertComplexPass& rp) : allocator (rp.allocator), module (rp.module) {}

    protected:
        AST::Expression& addCastIfRequired (AST::Expression& e, const Type& targetType)
        {
            auto sourceType = e.getResultType();

            if (sourceType.isEqual (targetType, Type::ComparisonFlags::ignoreConst | Type::ComparisonFlags::ignoreReferences))
                return e;

            if (sourceType.isComplex())
            {
                // Cast the real/imaginary components to the targetType
                auto& args = allocator.allocate<AST::CommaSeparatedList> (e.context);
                args.items.push_back (allocator.allocate<AST::DotOperator> (e.context, e, identifierFromString (allocator, e.context, "real")));
                args.items.push_back (allocator.allocate<AST::DotOperator> (e.context, e, identifierFromString (allocator, e.context, "imag")));

                return allocator.allocate<AST::TypeCast> (e.context, targetType.removeReferenceIfPresent(), args);
            }

            return allocator.allocate<AST::TypeCast> (e.context, targetType.removeReferenceIfPresent(), e);
        }

        static bool requiresRemapping (const soul::Type& type)
        {
            if (type.isComplex())
                return true;

            if (type.isVector() && type.getVectorElementType().isComplex())
                return true;

            if (type.isArray() && type.getArrayElementType().isComplex())
                return true;

            return false;
        }

        AST::Allocator& allocator;
        AST::ModuleBase& module;
    };

    //==============================================================================
    struct ConvertComplexElementAccess  : ComplexBase
    {
        ConvertComplexElementAccess (ConvertComplexPass& rp) : ComplexBase (rp) {}

        using super = RewritingASTVisitor;
        static inline constexpr const char* getPassName()  { return "ConvertComplexElementAccess"; }

        AST::Expression& visit (AST::Assignment& a) override
        {
            if (a.isResolved() && requiresRemapping (a.getResultType()))
            {
                a.newValue = addCastIfRequired (a.newValue, a.getResultType());

                if (auto v = cast<AST::ArrayElementRef> (a.target))
                {
                    if (v->object->getResultType().isVector())
                    {
                        auto& functionName = allocator.allocate<AST::QualifiedIdentifier> (a.context, soul::IdentifierPath::fromString (allocator.identifiers, "setElement"));
                        auto& args = allocator.allocate<AST::CommaSeparatedList> (a.context);
                        args.items.push_back (*v->object);
                        args.items.push_back (*v->startIndex);
                        args.items.push_back (a.newValue);

                        auto& call = allocator.allocate<AST::CallOrCast> (functionName, args, true);

                        return super::visit (call);
                    }
                }
            }

            super::visit (a);
            return a;
        }

        AST::Statement& visit (AST::ReturnStatement& r) override
        {
            super::visit (r);

            auto returnTypeExp = r.getParentFunction()->returnType;

            if (AST::isResolvedAsType (returnTypeExp) && requiresRemapping (returnTypeExp->resolveAsType()))
                r.returnValue = addCastIfRequired (*r.returnValue, returnTypeExp->resolveAsType());

            return r;
        }

        AST::Expression& visit (AST::TypeCast& t) override
        {
            super::visit (t);

            if (requiresRemapping (t.targetType))
                if (t.source->isResolved() && requiresRemapping (t.source->getResultType()))
                    return addCastIfRequired (t.source, t.targetType);

            return t;
        }

        AST::Expression& visit (AST::ArrayElementRef& r) override
        {
            super::visit (r);

            if (r.isResolved() && requiresRemapping (r.getResultType()) && r.object->getResultType().isVector())
            {
                // Convert this to a method call
                auto& functionName = allocator.allocate<AST::QualifiedIdentifier> (r.context, soul::IdentifierPath::fromString (allocator.identifiers, "getElement"));
                auto& args = allocator.allocate<AST::CommaSeparatedList> (r.context);
                args.items.push_back (*r.object);
                args.items.push_back (*r.startIndex);

                return allocator.allocate<AST::CallOrCast> (functionName, args, true);
            }

            return r;
        }
    };

    //==============================================================================
    struct ConvertComplexOperators  : public ComplexBase
    {
        ConvertComplexOperators (ConvertComplexPass& rp) : ComplexBase (rp) {}

        using super = RewritingASTVisitor;
        static inline constexpr const char* getPassName()  { return "ConvertComplexOperators"; }

        AST::Expression& visit (AST::ComplexMemberRef& s) override
        {
            super::visit (s);

            if (auto v = cast<AST::ArrayElementRef> (s.object))
            {
                if (! v->object->isResolved() || v->object->getResultType().isVector())
                {
                    // Convert a[b].c to a.c[b]
                    auto& memberRef = allocator.allocate<AST::DotOperator> (s.context, *v->object, identifierFromString (allocator, s.context, s.memberName));
                    return allocator.allocate<AST::ArrayElementRef> (s.context, memberRef, v->startIndex, v->endIndex, v->isSlice);
                }
            }

            // Convert back to a dot operator, so that the subsequent resolution pass will convert it to the right struct member access
            return allocator.allocate<AST::DotOperator> (s.context, s.object, identifierFromString (allocator, s.context, s.memberName));
        }

        AST::Expression& visit (AST::UnaryOperator& u) override
        {
            RewritingASTVisitor::visit (u);

            if (u.isResolved())
            {
                if (requiresRemapping (u.getResultType()))
                {
                    // Convert to a function call
                    auto functionName = getFunctionNameForOperator (u);
                    auto& args = allocator.allocate<AST::CommaSeparatedList> (u.context);
                    args.items.push_back (u.source);

                    return allocator.allocate<AST::CallOrCast> (functionName, args, true);
                }
            }

            return u;
        }

        AST::Expression& visit (AST::BinaryOperator& b) override
        {
            RewritingASTVisitor::visit (b);

            if (b.isResolved())
            {
                if (requiresRemapping (b.getOperandType()))
                {
                    // Convert to a function call
                    auto functionName = getFunctionNameForOperator (b);
                    auto& args = allocator.allocate<AST::CommaSeparatedList> (b.context);
                    args.items.push_back (addCastIfRequired (b.lhs, b.getOperandType()));
                    args.items.push_back (addCastIfRequired (b.rhs, b.getOperandType()));

                    return allocator.allocate<AST::CallOrCast> (functionName, args, true);
                }
            }

            return b;
        }

    private:
        pool_ref<AST::QualifiedIdentifier> getFunctionNameForOperator (AST::UnaryOperator& u)
        {
            std::string functionName;

            switch (u.operation)
            {
                case UnaryOp::Op::negate:       functionName = "negate"; break;

                default:
                    u.context.throwError (soul::Errors::wrongTypeForUnary());
            }

            return allocator.allocate<AST::QualifiedIdentifier> (u.context, soul::IdentifierPath::fromString (allocator.identifiers, functionName));
        }

        pool_ref<AST::QualifiedIdentifier> getFunctionNameForOperator (AST::BinaryOperator& b)
        {
            std::string functionName;

            switch (b.operation)
            {
                case BinaryOp::Op::add:         functionName = "add";        break;
                case BinaryOp::Op::subtract:    functionName = "subtract";   break;
                case BinaryOp::Op::multiply:    functionName = "multiply";   break;
                case BinaryOp::Op::divide:      functionName = "divide";     break;
                case BinaryOp::Op::equals:      functionName = "equals";     break;
                case BinaryOp::Op::notEquals:   functionName = "notEquals";  break;

                default:
                    b.context.throwError (soul::Errors::illegalTypesForBinaryOperator (BinaryOp::getSymbol (b.operation),
                                                                                       b.lhs->getResultType().getDescription(),
                                                                                       b.rhs->getResultType().getDescription()));
            }

            return allocator.allocate<AST::QualifiedIdentifier> (b.context, soul::IdentifierPath::fromString (allocator.identifiers, functionName));
        }
    };

    //==============================================================================
    struct ConvertComplexRemapTypes  : public ComplexBase
    {
        ConvertComplexRemapTypes (ConvertComplexPass& rp)  : ComplexBase (rp)
        {
            soulLib = getModule (soul::IdentifierPath::fromString (allocator.identifiers, "soul"));
        }

        using super = RewritingASTVisitor;
        static inline constexpr const char* getPassName()  { return "ConvertComplexRemapTypes"; }

        AST::ModuleBase* soulLib;

        AST::StructDeclaration* structDeclaration = nullptr;

        AST::Expression& visit (AST::ConcreteType& t) override
        {
            super::visit (t);

            if (requiresRemapping (t.type))
            {
                if (structDeclaration != nullptr)
                    structDeclaration->structureMembersUpdated();

                return getRemappedType (t.context, t.type);
            }

            return t;
        }

        AST::StructDeclaration& visit (AST::StructDeclaration& s) override
        {
            structDeclaration = &s;
            super::visit (s);
            structDeclaration = nullptr;

            return s;
        }

        AST::Expression& visit (AST::TypeCast& t) override
        {
            super::visit (t);

            if (requiresRemapping (t.targetType))
            {
                auto& remappedType = getRemappedType (t.context, t.targetType);

                if (auto args = cast<AST::CommaSeparatedList> (t.source))
                    return allocator.allocate<AST::CallOrCast> (remappedType, args, false);

                auto& args = allocator.allocate<AST::CommaSeparatedList> (t.context);
                args.items.push_back (t.source);
                args.items.push_back (allocator.allocate<AST::Constant> (t.context, soul::Value::createInt32 (0)));

                return allocator.allocate<AST::CallOrCast> (remappedType, args, false);
            }

            return t;
        }

        AST::Expression& visit (AST::Constant& c) override
        {
            super::visit (c);

            if (requiresRemapping (c.getResultType()))
            {
                auto& remappedType = getRemappedType (c.context, c.getResultType());
                auto& args = allocator.allocate<AST::CommaSeparatedList> (c.context);
                auto resultType = c.getResultType();

                if (c.getResultType().isComplex32())
                {
                    if (resultType.isVector())
                    {
                        ArrayWithPreallocation<Value, 8> realValues, imagValues;

                        for (size_t i = 0; i < resultType.getVectorSize(); i++)
                        {
                            auto v = c.value.getSlice (i, i + 1).getAsComplex32();

                            realValues.push_back (soul::Value (v.real()));
                            imagValues.push_back (soul::Value (v.imag()));
                        }

                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value::createArrayOrVector (soul::Type::createVector (PrimitiveType::float32, resultType.getVectorSize()), realValues)));
                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value::createArrayOrVector (soul::Type::createVector (PrimitiveType::float32, resultType.getVectorSize()), imagValues)));
                    }
                    else
                    {
                        auto v = c.value.getAsComplex32();
                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value (v.real())));
                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value (v.imag())));
                    }
                }
                else
                {
                    if (resultType.isVector())
                    {
                        ArrayWithPreallocation<Value, 8> realValues, imagValues;

                        for (size_t i = 0; i < resultType.getVectorSize(); i++)
                        {
                            auto v = c.value.getSlice (i, i + 1).getAsComplex64();

                            realValues.push_back (soul::Value (v.real()));
                            imagValues.push_back (soul::Value (v.imag()));
                        }

                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value::createArrayOrVector (soul::Type::createVector (PrimitiveType::float64, resultType.getVectorSize()), realValues)));
                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value::createArrayOrVector (soul::Type::createVector (PrimitiveType::float64, resultType.getVectorSize()), imagValues)));
                    }
                    else
                    {
                        auto v = c.value.getAsComplex64();
                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value (v.real())));
                        args.items.push_back (allocator.allocate<AST::Constant> (c.context, soul::Value (v.imag())));
                    }
                }

                return allocator.allocate<AST::CallOrCast> (remappedType, args, false);
            }

            return c;
        }

    private:
        std::vector<std::string> createdNamespaceAliases;

        AST::Expression& getRemappedType (AST::Context& context, const soul::Type& type)
        {
            if (type.isPrimitive())
                return getRemappedType (context, type.isComplex32(), 1, 0, type.isReference(), type.isConst());

            if (type.isVector())
                return getRemappedType (context, type.isComplex32(), type.getVectorSize(), 0, type.isReference(), type.isConst());

            SOUL_ASSERT (type.isArray());
            return getRemappedType (context,
                                    type.getArrayElementType().isComplex32(),
                                    type.getArrayElementType().getVectorSize(),
                                    type.getArraySize(),
                                    type.isReference(),
                                    type.isConst());
        }

        AST::Expression& getRemappedType (AST::Context& context, bool is32Bit, size_t vectorSize, size_t arraySize, bool isReference, bool isConst)
        {
            // Build the namespace path
            int bits = is32Bit ? 32 : 64;

            std::string namespaceAlias = "complex_lib" + std::to_string (bits) + "_" + std::to_string (vectorSize);
            std::string namespacePath = "soul::" + namespaceAlias;

            if (! soul::contains (createdNamespaceAliases, namespaceAlias))
            {
                // Create the namespace alias
                auto& specialisationArgs = allocator.allocate<AST::CommaSeparatedList> (context);
                specialisationArgs.items.push_back (allocator.allocate<AST::ConcreteType> (context, is32Bit ? PrimitiveType::float32 : PrimitiveType::float64));
                specialisationArgs.items.push_back (allocator.allocate<AST::Constant> (context, soul::Value (static_cast<int32_t> (vectorSize))));

                auto& n = allocator.allocate<AST::NamespaceAliasDeclaration> (context,
                                                                              allocator.get (namespaceAlias),
                                                                              allocator.allocate<AST::QualifiedIdentifier> (context, soul::IdentifierPath::fromString (allocator.identifiers, "soul::complex_lib")),
                                                                              specialisationArgs);

                soulLib->namespaceAliases.push_back (n);
                createdNamespaceAliases.push_back (namespaceAlias);
            }

            pool_ref<AST::Expression> expr = allocator.allocate<AST::QualifiedIdentifier> (context, soul::IdentifierPath::fromString (allocator.identifiers,
                                                                                                                                      namespacePath + "::ComplexType"));

            if (arraySize != 0)
            {
                auto& arraySizeConstant = allocator.allocate<AST::Constant> (context, soul::Value (static_cast<int32_t> (arraySize)));
                expr = allocator.allocate<AST::SubscriptWithBrackets> (context, expr, arraySizeConstant);
            }

            if (isReference)
                expr = allocator.allocate<AST::TypeMetaFunction> (context, expr, AST::TypeMetaFunction::Op::makeReference);

            if (isConst)
                expr = allocator.allocate<AST::TypeMetaFunction> (context, expr, AST::TypeMetaFunction::Op::makeConst);

            return expr;
        }

        AST::ModuleBase* getModule (const soul::IdentifierPath& path) const
        {
            AST::Scope::NameSearch search;

            search.partiallyQualifiedPath = path;
            search.findNamespaces = true;

            module.performFullNameSearch (search, nullptr);

            if (search.itemsFound.size() == 1)
            {
                auto item = search.itemsFound.front();

                if (auto e = cast<AST::ModuleBase> (item))
                    return e.get();
            }

            SOUL_ASSERT_FALSE;
            return {};
        }
    };
};

} // namespace soul
