// http://bugzilla.redhat.com/show_bug.cgi?id=547286
// { dg-do compile }
// { dg-options "-Os -w" }

struct A
{
  void dispose ();
  void release () { dispose (); }
};
struct B
{
  A *pi_;
  B () { pi_ = __null; }
  ~B () { pi_->release (); }
  B (B const &) : pi_ () {}
};
template <class>
struct C
{
  B pn;
};
struct D
{
  char *getStr ();
};
struct E
{
};
D EToD (E, unsigned = 0x4406U);
struct F;
enum G
{
  GX1,
  GX2,
  GX3,
  GX4,
  GX5,
  GX6,
  GX7
};
struct H;
typedef C <H> I;
struct J {};
template <typename T, bool, bool>
struct K
{
  typedef T param_type;
};
template <typename T>
struct L
{
  typedef T const_reference;
  typedef typename K <T, false, false>::param_type param_type;
};
struct M;
template <bool, typename, typename>
struct N;
template <typename T1, typename T2>
struct N <false, T1, T2>
{
  typedef T2 type;
};
template <typename T1 = M, typename T2 = M, typename T3 = M>
struct O
{
  typedef N <static_cast <bool> (T1::value), T2, T3> almost_type_;
  typedef typename almost_type_::type type;
};
template <typename T>
struct P
{
  typedef T type;
};
template <typename>
struct Q
{
};
template <typename T>
struct R
{
  typedef T *TP;
  typedef TP R::*type;
};
template <typename DerivedT, typename BaseT = Q <DerivedT> >
struct S : BaseT
{
  typedef R <DerivedT> impl_t;
  typedef typename impl_t::type bool_type;
  operator bool_type ();
};
template <typename = J>
struct U : public S <U <> >
{
};
struct parser_tag_base
{
};
struct parser_address_tag : parser_tag_base
{
};
struct iteration_policy;
struct action_policy;
struct U_policy;
template <typename = iteration_policy, typename = U_policy, typename = action_policy>
struct scanner_policies;
template <typename = char *, typename = scanner_policies <> >
struct scanner;
struct iteration_policy
{
};
struct U_policy
{
  template <typename>
  struct result
  {
    typedef U <> type;
  };
};
template <typename MatchPolicyT, typename T>
struct U_result
{
  typedef typename MatchPolicyT::template result <T>::type type;
};
struct action_policy
{
};
template <typename IterationPolicyT, typename MatchPolicyT, typename ActionPolicyT>
struct scanner_policies:public IterationPolicyT, MatchPolicyT, ActionPolicyT
{
};
struct scanner_base;
template <typename IteratorT, typename PoliciesT>
struct scanner : public PoliciesT
{
  typedef typename L <IteratorT>::param_type iter_param_t;
  scanner (IteratorT, iter_param_t, PoliciesT = PoliciesT ()) : PoliciesT (), first (), last () {}
  IteratorT first;
  IteratorT last;
};
template <typename, typename>
struct action;
template <typename ParserT, typename ScannerT>
struct parser_result
{
  typedef typename P <ParserT>::type parser_type;
  typedef typename parser_type::template result <ScannerT>::type type;
};
template <typename DerivedT>
struct parser
{
  typedef DerivedT embed_t;
  template <typename ScannerT>
  struct result
  {
    typedef typename U_result <ScannerT, J>::type type;
  };
  DerivedT derived () const;
  template <typename ActionT>
  action <DerivedT, ActionT> operator[] (ActionT);
};
template <typename = char *>
struct parse_info
{
};
struct space_parser;
template <typename SkipT>
struct phrase_parser
{
  template <typename IteratorT, typename ParserT>
  static parse_info <> parse (IteratorT first_, IteratorT last, ParserT p, space_parser const &)
  {
    typedef scanner <> scanner_t;
    IteratorT first = first_;
    scanner_t scan (first, last);
    U <> hit = p.parse (scan);
    return parse_info <> ();
  }
};
template <typename IteratorT, typename ParserT, typename SkipT>
parse_info <> parse (IteratorT first, IteratorT last, parser <ParserT> p, parser <SkipT> skip)
{
  phrase_parser <SkipT>::parse (first, last, p.derived (), skip.derived ());
  return parse_info <> ();
}
template <typename DerivedT>
struct char_parser : public parser <DerivedT>
{
};
template <typename = char>
struct chlit : public char_parser <chlit <> >
{
};
template <typename = char *>
struct strlit : public parser <strlit <> >
{
};
template <typename CharT>
strlit <CharT *> str_p (CharT)
{
  return strlit <CharT *> ();
}
struct space_parser : public char_parser <space_parser>
{
};
space_parser space_p = space_parser ();
struct end_parser : public parser <end_parser>
{
};
end_parser end_p = end_parser ();
template <class, class, bool, bool, bool>
struct compressed_pair_switch;
template <class T1, class T2>
struct compressed_pair_switch <T1, T2, false, false, false>
{
  static const int value = 0;
};
template <class, class, int>
struct compressed_pair_imp;
template <class T1, class T2>
struct compressed_pair_imp <T1, T2, 0>
{
  typedef T1 first_type;
  typedef T2 second_type;
  typedef typename L <first_type>::param_type first_param_type;
  typedef typename L <second_type>::param_type second_param_type;
  compressed_pair_imp (first_param_type x, second_param_type y) : first_ (x), second_ (y) {}
  first_type first_;
  second_type second_;
};
template <class T1, class T2>
struct compressed_pair : private compressed_pair_imp <T1, T2, compressed_pair_switch <T1, T2, false, false, false>::value>
{
  typedef compressed_pair_imp <T1, T2, compressed_pair_switch <T1, T2, false, false, false>::value> base;
  typedef T1 first_type;
  typedef T2 second_type;
  typedef typename L <first_type>::param_type first_param_type;
  typedef typename L <second_type>::param_type second_param_type;
  compressed_pair (first_param_type x, second_param_type y):base (x, y) {}
};
template <typename S, typename BaseT>
struct unary : public BaseT
{
  typedef typename S::embed_t subject_embed_t;
  subject_embed_t subj;
};
template <typename A, typename B, typename BaseT>
struct binary : public BaseT
{
  typedef BaseT base_t;
  typedef typename L <A>::param_type left_param_t;
  typedef typename L <A>::const_reference left_return_t;
  typedef typename L <B>::param_type right_param_t;
  typedef typename A::embed_t left_embed_t;
  typedef typename B::embed_t right_embed_t;
  binary (left_param_t a, right_param_t b) : base_t (), subj (a, b) {}
  left_return_t left ();
  compressed_pair <left_embed_t, right_embed_t> subj;
};
struct parser_context_base
{
};
template <typename>
struct parser_context_linker;
template <typename AttrT = J>
struct parser_context : parser_context_base
{
  typedef AttrT attr_t;
  typedef parser_context_linker <parser_context <> > context_linker_t;
};
template <typename ContextT, typename DerivedT>
struct context_aux : public ContextT::base_t::template aux <DerivedT>
{
};
template <typename ContextT>
struct parser_context_linker : ContextT
{
};
template <class T>
struct scoped_ptr
{
  T *px;
  scoped_ptr (T * = 0) {}
};
template < typename Base, typename Derived > struct is_base_and_derived
{
  static const bool value = false;
};
template <typename BaseT, typename DefaultT, typename T0, typename T1, typename T2>
struct get_param
{
  typedef typename O <is_base_and_derived <BaseT, T0>, T0, typename O <is_base_and_derived <BaseT, T1>, T1, typename O <is_base_and_derived <BaseT, T2>, T2, DefaultT>::type>::type>::type type;
};
template <typename T0, typename T1, typename T2>
struct get_context
{
  typedef typename get_param <parser_context_base, parser_context <>, T0, T1, T2>::type type;
};
template <typename T0, typename T1, typename T2>
struct get_tag
{
  typedef typename get_param <parser_tag_base, parser_address_tag, T0, T1, T2>::type type;
};
template <typename T0, typename T1, typename T2>
struct get_scanner
{
  typedef typename get_param <scanner_base, scanner <>, T0, T1, T2>::type type;
};
template <typename DerivedT, typename, typename T0, typename T1, typename T2>
struct rule_base : parser <DerivedT>, get_tag <T0, T1, T2>::type
{
  typedef typename get_scanner <T0, T1, T2>::type scanner_t;
  typedef typename get_context <T0, T1, T2>::type context_t;
  typedef typename context_t::context_linker_t linked_context_t;
  typedef typename linked_context_t::attr_t attr_t;
};
template <typename ScannerT, typename AttrT>
struct abstract_parser
{
  virtual ~abstract_parser () {}
};
template <typename ParserT, typename ScannerT, typename AttrT>
struct concrete_parser : abstract_parser <ScannerT, AttrT>
{
  concrete_parser (ParserT const &p) : p (p) {}
  virtual abstract_parser <ScannerT, AttrT> *
  clone ()
  {
    return new concrete_parser (p);
  }
  typename ParserT::embed_t p;
};
template <typename T0, typename T1 = J, typename T2 = J>
struct rule : rule_base <rule <T2>, rule <T0>, T0, T1, T2>
{
  typedef rule <T2> base_t;
  typedef typename base_t::scanner_t scanner_t;
  typedef typename base_t::attr_t attr_t;
  typedef abstract_parser <scanner_t, attr_t> abstract_parser_t;
  rule () : ptr () {}
  template <typename ParserT>
  rule (ParserT const &p) : ptr (new concrete_parser <ParserT, scanner_t, attr_t> (p))
  {
  }
  scoped_ptr <abstract_parser_t> ptr;
};
template <typename, typename>
struct grammar;
template <typename DerivedT, typename ContextT, typename ScannerT>
typename DerivedT::template definition <ScannerT> get_definition (grammar <DerivedT, ContextT> *self)
{
  typedef typename DerivedT::template definition <ScannerT> definition_t;
  definition_t d (self->derived ());
  return d;
}
template <int, typename DerivedT, typename ContextT, typename ScannerT>
typename parser_result <grammar <DerivedT, ContextT>, ScannerT>::type
grammar_parser_parse (grammar < DerivedT, ContextT > *self, ScannerT)
{
  typedef typename parser_result <grammar <DerivedT, ContextT>, ScannerT>::type result_t;
  get_definition <DerivedT, ContextT, ScannerT> (self);
  return result_t ();
}
template <typename DerivedT, typename ContextT = parser_context <> >
struct grammar : parser <DerivedT>
{
  typedef grammar <DerivedT> self_t;
  template <typename ScannerT>
  typename parser_result <self_t, ScannerT>::type parse_main (ScannerT & scan)
  {
    typedef typename parser_result <self_t, ScannerT>::type tt;
    grammar_parser_parse <0> (this, scan);
    return tt ();
  }
  template <typename ScannerT>
  typename parser_result <self_t, ScannerT>::type parse (ScannerT & scan)
  {
    return parse_main (scan);
  }
};
template <typename ParserT, typename ActionT>
struct action : unary <ParserT, parser <action <ParserT, ActionT> > >
{
  ActionT actor;
};
template <typename A, typename B>
struct sequence : binary <A, B, parser <sequence <A, B> > >
{
  typedef sequence <A, B> self_t;
  template <typename ScannerT>
  typename parser_result <self_t, ScannerT>::type parse (ScannerT & scan)
  {
    typedef typename parser_result <self_t, ScannerT>::type result_t;
    if (result_t ma = this->left ().parse (scan))
      {
      }
    return this->left ().parse (scan);
  }
};
template <typename A, typename B>
sequence <A, B> operator>> (parser <A>, parser <B>);
template <typename B>
sequence <chlit <>, B> operator>> (char, parser <B>);
template <typename A, typename B>
struct alternative : binary <A, B, parser <alternative <A, B> > >
{
  typedef alternative <A, B> self_t;
  typedef binary <A, B, parser <self_t> > base_t;
  alternative (A const &a, B const &b):
  base_t (a, b) {}
};
template <typename A, typename B>
alternative <A, B> operator| (parser <A> const &a, parser <B> const &b)
{
  alternative <A, B> c (a.derived (), b.derived ());
  return c;
}
namespace
{
  typedef char *StringIteratorT;
  struct ParserContext;
  typedef C <ParserContext> ParserContextSharedPtr;
  struct EnumFunctor
  {
    G meFunct;
    double mnValue;
    ParserContextSharedPtr mpContext;
    EnumFunctor (G, ParserContextSharedPtr):
    mpContext () {}
  };
  struct BinaryFunctionFunctor
  {
    G meFunct;
    ParserContextSharedPtr mpContext;
    BinaryFunctionFunctor (G, ParserContextSharedPtr):
    mpContext () {}
  };
  struct ExpressionGrammar : grammar <ExpressionGrammar>
  {
    ExpressionGrammar (ParserContextSharedPtr) {}
    template <typename ScannerT>
    struct definition
    {
      definition (ExpressionGrammar self)
      {
	id = str_p ("pi")[EnumFunctor (GX1, self.getContext ())] |
	     str_p ("left")[EnumFunctor (GX2, self.getContext ())] |
	     str_p ("bottom")[EnumFunctor (GX3, self.getContext ())] |
	     str_p ("xstretch")[EnumFunctor (GX4, self.getContext ())] |
	     str_p ("ystretch")[EnumFunctor (GX5, self.getContext ())] |
	     modifierReference |
	     (('+' >> multiplicativeExpression)[BinaryFunctionFunctor (GX6, self.getContext ())] |
	      ('-' >> multiplicativeExpression)[BinaryFunctionFunctor (GX7, self.getContext ())]);
      }
      rule <ScannerT> multiplicativeExpression;
      rule <ScannerT> modifierReference;
      rule <ScannerT> id;
    };
    ParserContextSharedPtr getContext ();
  };
}
void
FunctionParser (E rFunction, F &)
{
  D rAsciiFunction (EToD (rFunction, 11U));
  StringIteratorT aStart (rAsciiFunction.getStr ());
  StringIteratorT aEnd;
  ParserContextSharedPtr pContext;
  ExpressionGrammar aExpressionGrammer (pContext);
  parse_info <> x (parse (aStart, aEnd, aExpressionGrammer >> end_p, space_p));
}
