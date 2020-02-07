// file      : mod/diagnostics.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#ifndef MOD_DIAGNOSTICS_HXX
#define MOD_DIAGNOSTICS_HXX

#include <sstream>
#include <exception> // uncaught_exception[s]()

#include <libbutl/ft/exception.hxx> // uncaught_exceptions

#include <libbrep/types.hxx>
#include <libbrep/utility.hxx>

namespace brep
{
  struct location
  {
    location (): line (0), column (0) {}
    location (string f, uint64_t l, uint64_t c)
        : file (move (f)), line (l), column (c) {}

    string file;
    uint64_t line;
    uint64_t column;
  };

  enum class severity {error, warning, info, trace};

  struct diag_entry
  {
    severity sev;
    const char* name {nullptr}; // E.g., a function name in tracing.
    location loc;
    string msg;
  };

  using diag_data = vector<diag_entry>;

  //
  //
  template <typename> struct diag_prologue;
  template <typename> struct diag_mark;

  using diag_epilogue = function<void (diag_data&&)>;

  struct diag_record
  {
    template <typename T>
    friend const diag_record&
    operator<< (const diag_record& r, const T& x)
    {
      r.os_ << x;
      return r;
    }

    diag_record ()
        :
#ifdef __cpp_lib_uncaught_exceptions
        uncaught_ (std::uncaught_exceptions ()),
#endif
        epilogue_ (nullptr) {}

    template <typename B>
    explicit
    diag_record (const diag_prologue<B>& p): diag_record () {*this << p;}

    template <typename B>
    explicit
    diag_record (const diag_mark<B>& m): diag_record () {*this << m;}

    ~diag_record () noexcept(false);

    void
    append (const diag_epilogue& e) const
    {
      if (epilogue_ == nullptr) // Keep the first epilogue (think 'fail').
        epilogue_ = &e;

      if (!data_.empty ())
      {
        data_.back ().msg = os_.str ();

        // Reset the stream. There got to be a more efficient way to do it.
        //
        os_.clear ();
        os_.str ("");
      }

      data_.push_back (diag_entry ());
    }

    diag_entry&
    current () const {return data_.back ();}

    // Move constructible-only type.
    //
    // Older versions of libstdc++ don't have the ostringstream move support
    // and accuratly detecting its version is non-trivial. So we always use
    // the pessimized implementation with libstdc++. Luckily, GCC doesn't seem
    // to be needing move due to copy/move elision.
    //
#ifdef __GLIBCXX__
    diag_record (diag_record&&);
#else
    diag_record (diag_record&& r)
        :
#ifdef __cpp_lib_uncaught_exceptions
        uncaught_ (r.uncaught_),
#endif
        data_ (move (r.data_)),
        os_ (move (r.os_)),
        epilogue_ (r.epilogue_)
    {
      r.data_.clear (); // Empty.
    }
#endif

    diag_record& operator= (diag_record&&) = delete;

    diag_record (const diag_record&) = delete;
    diag_record& operator= (const diag_record&) = delete;

  private:
#ifdef __cpp_lib_uncaught_exceptions
    const int uncaught_;
#endif
    mutable diag_data data_;
    mutable std::ostringstream os_;
    mutable const diag_epilogue* epilogue_;
  };

  // Base (B) should provide operator() that configures diag_record.
  //
  template <typename B>
  struct diag_prologue: B
  {
    diag_prologue (const diag_epilogue& e): B (), epilogue_ (e) {}

    template <typename... A>
    diag_prologue (const diag_epilogue& e, A&&... a)
        : B (forward<A> (a)...), epilogue_ (e) {}

    template <typename T>
    diag_record
    operator<< (const T& x) const
    {
      diag_record r;
      r.append (epilogue_);
      B::operator() (r);
      r << x;
      return r;
    }

    friend const diag_record&
    operator<< (const diag_record& r, const diag_prologue& p)
    {
      r.append (p.epilogue_);
      p (r);
      return r;
    }

  private:
    const diag_epilogue& epilogue_;
  };

  // Base (B) should provide operator() that returns diag_prologue.
  //
  template <typename B>
  struct diag_mark: B
  {
    diag_mark (): B () {}

    template <typename... A>
    diag_mark (A&&... a): B (forward<A> (a)...) {}

    template <typename T>
    diag_record
    operator<< (const T& x) const
    {
      return B::operator() () << x;
    }

    friend const diag_record&
    operator<< (const diag_record& r, const diag_mark& m)
    {
      return r << m ();
    }
  };

  // Prologues.
  //
  struct simple_prologue_base
  {
    explicit
    simple_prologue_base (severity s, const char* name)
        : sev_ (s), name_ (name) {}

    void
    operator() (const diag_record& r) const
    {
      diag_entry& e (r.current ());
      e.sev = sev_;
      e.name = name_;
    }

  private:
    severity sev_;
    const char* name_;
  };
  typedef diag_prologue<simple_prologue_base> simple_prologue;

  struct location_prologue_base
  {
    location_prologue_base (severity s,
                            const char* name,
                            const location& l)
        : sev_ (s), name_ (name), loc_ (l) {}

    void
    operator() (const diag_record& r) const
    {
      diag_entry& e (r.current ());
      e.sev = sev_;
      e.name = name_;
      e.loc = loc_; //@@ I think we can probably move it.
    }

  private:
    severity sev_;
    const char* name_;
    const location loc_;
  };
  typedef diag_prologue<location_prologue_base> location_prologue;

  // Marks.
  //
  struct basic_mark_base
  {
    explicit
    basic_mark_base (severity s,
                     const diag_epilogue& e,
                     const char* name = nullptr,
                     const void* data = nullptr)
        : sev_ (s), epilogue_ (e), name_ (name), data_ (data) {}

    simple_prologue
    operator() () const
    {
      return simple_prologue (epilogue_, sev_, name_);
    }

    location_prologue
    operator() (const location& l) const
    {
      return location_prologue (epilogue_, sev_, name_, l);
    }

    template <typename L>
    location_prologue
    operator() (const L& l) const
    {
      // get_location() is the user-supplied ADL-searched function.
      //
      return location_prologue (
        epilogue_, sev_, name_, get_location (l, data_));
    }

  private:
    severity sev_;
    const diag_epilogue& epilogue_;
    const char* name_;
    const void* data_;
  };
  using basic_mark = diag_mark<basic_mark_base>;

  template <typename E>
  struct fail_mark_base
  {
    explicit
    fail_mark_base (const char* name = nullptr, const void* data = nullptr)
        : name_ (name), data_ (data) {}

    simple_prologue
    operator() () const
    {
      return simple_prologue (epilogue_, severity::error, name_);
    }

    location_prologue
    operator() (const location& l) const
    {
      return location_prologue (epilogue_, severity::error, name_, l);
    }

    template <typename L>
    location_prologue
    operator() (const L& l) const
    {
      return location_prologue (
        epilogue_, severity::error, name_, get_location (l, data_));
    }

    static void
    epilogue (diag_data&& d) {throw E (move (d));}

  private:
    const diag_epilogue epilogue_ {&epilogue};
    const char* name_;
    const void* data_;
  };

  template <typename E>
  using fail_mark = diag_mark<fail_mark_base<E>>;
}

#endif // MOD_DIAGNOSTICS_HXX
