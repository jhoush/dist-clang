// RUN: %clang_cc1 -fsyntax-only -verify %s
class X { 
public:
  operator bool();
  operator int() const;

  bool f() {
    return operator bool();
  }

  float g() {
    return operator float(); // expected-error{{use of undeclared 'operator float'}}
  }
};

operator int(); // expected-error{{conversion function must be a non-static member function}}

operator int; // expected-error{{'operator int' cannot be the name of a variable or data member}}

typedef int func_type(int);
typedef int array_type[10];

class Y {
public:
  void operator bool(int, ...) const; // expected-error{{conversion function cannot have a return type}} \
  // expected-error{{conversion function cannot have any parameters}}
  
  operator float(...) const;  // expected-error{{conversion function cannot be variadic}}
  
  
  operator func_type(); // expected-error{{conversion function cannot convert to a function type}}
  operator array_type(); // expected-error{{conversion function cannot convert to an array type}}
};


typedef int INT;
typedef INT* INT_PTR;

class Z { 
  operator int(); // expected-note {{previous declaration is here}}
  operator int**(); // expected-note {{previous declaration is here}}
  
  operator INT();  // expected-error{{conversion function cannot be redeclared}}
  operator INT_PTR*(); // expected-error{{conversion function cannot be redeclared}}
};


class A { };

class B : public A {
public:
  operator A&() const; // expected-warning{{conversion function converting 'B' to its base class 'A' will never be used}}
  operator const void() const; // expected-warning{{conversion function converting 'B' to 'void const' will never be used}}
  operator const B(); // expected-warning{{conversion function converting 'B' to itself will never be used}}
};

// This used to crash Clang.
struct Flip;
struct Flop { // expected-note{{candidate is the implicit copy constructor}}
  Flop();
  Flop(const Flip&); // expected-note{{candidate constructor}}
};
struct Flip {
  operator Flop() const; // expected-note{{candidate function}}
};
Flop flop = Flip(); // expected-error {{conversion from 'Flip' to 'Flop' is ambiguous}}

// This tests that we don't add the second conversion declaration to the list of user conversions
struct C {
  operator const char *() const;
};

C::operator const char*() const { return 0; }

void f(const C& c) {
  const char* v = c;
}

// Test. Conversion in base class is visible in derived class.
class XB { 
public:
  operator int(); // expected-note {{candidate function}}
};

class Yb : public XB { 
public:
  operator char(); // expected-note {{candidate function}}
};

void f(Yb& a) {
  if (a) { } // expected-error {{conversion from 'Yb' to 'bool' is ambiguous}}
  int i = a; // OK. calls XB::operator int();
  char ch = a;  // OK. calls Yb::operator char();
}

// Test conversion + copy construction.
class AutoPtrRef { };

class AutoPtr {
  // FIXME: Using 'unavailable' since we do not have access control yet.
  // FIXME: The error message isn't so good.
  AutoPtr(AutoPtr &) __attribute__((unavailable)); // expected-note{{explicitly marked}}
  
public:
  AutoPtr();
  AutoPtr(AutoPtrRef);
  
  operator AutoPtrRef();
};

AutoPtr make_auto_ptr();

AutoPtr test_auto_ptr(bool Cond) {
  AutoPtr p1( make_auto_ptr() );
  
  AutoPtr p;
  if (Cond)
    return p; // expected-error{{call to deleted constructor}}
  
  return AutoPtr();
}

struct A1 {
  A1(const char *);
  ~A1();

private:
  A1(const A1&) __attribute__((unavailable)); // expected-note{{here}}
};

A1 f() {
  return "Hello"; // expected-error{{invokes deleted copy constructor}}
}

namespace source_locations {
  template<typename T>
  struct sneaky_int {
    typedef int type;
  };

  template<typename T, typename U>
  struct A { };

  template<typename T>
  struct A<T, T> : A<T, int> { };

  struct E {
    template<typename T>
    operator A<T, typename sneaky_int<T>::type>&() const; // expected-note{{candidate function}}
  };

  void f() {
    A<float, float> &af = E(); // expected-error{{no viable conversion}}
    A<float, int> &af2 = E();
    const A<float, int> &caf2 = E();
  }

  // Check 
  template<typename T>
  struct E2 {
    operator T
    * // expected-error{{pointer to a reference}}
    () const;
  };

  E2<int&> e2i; // expected-note{{in instantiation}}
}

namespace crazy_declarators {
  struct A {
    (&operator bool())(); // expected-error {{must use a typedef to declare a conversion to 'bool (&)()'}}

    // FIXME: This diagnostic is misleading (the correct spelling
    // would be 'operator int*'), but it's a corner case of a
    // rarely-used syntax extension.
    *operator int();  // expected-error {{must use a typedef to declare a conversion to 'int *'}}
  };
}
