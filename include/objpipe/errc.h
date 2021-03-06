#ifndef OBJPIPE_ERRC_H
#define OBJPIPE_ERRC_H

///\file
///\ingroup objpipe objpipe_errors

#include <string>
#include <system_error>
#include <type_traits>
#include <iosfwd>
#include <ostream>

namespace objpipe {


/**
 * \brief Object pipe error conditions.
 * \ingroup objpipe_errors
 */
enum class objpipe_errc {
  success=0, ///< Status code indicating successful completion.
  closed, ///< Status code indicating failure, due to a closed object pipe.
  bad, ///< Status code indicating the objpipe went bad.
  no_thread ///< Status code indicating the objpipe has no emitting thread.
};


} /* namespace objpipe */

namespace std {


template<>
struct is_error_condition_enum<objpipe::objpipe_errc>
: true_type
{};


} /* namespace std */

namespace objpipe {


/**
 * \brief Reference to the \ref objpipe_category().
 * \ingroup objpipe_errors
 * \return the object pipe error category.
 */
inline const std::error_category& objpipe_category() {
  class objpipe_category_t
  : public std::error_category
  {
   public:
    const char* name() const noexcept override {
      return "objpipe";
    }

    std::error_condition default_error_condition(int e) const noexcept override {
      return std::error_condition(objpipe_errc(e));
    }

    bool equivalent(const std::error_code& ec, int e) const noexcept override {
      return &ec.category() == this && ec.value() == e;
    }

    std::string message(int e) const override {
      using std::to_string;

      switch (objpipe_errc(e)) {
        default:
          return "objpipe unknown error " + to_string(e);
        case objpipe_errc::success:
          return "success";
        case objpipe_errc::closed:
          return "objpipe closed";
        case objpipe_errc::bad:
          return "objpipe bad";
        case objpipe_errc::no_thread:
          return "objpipe source has no emitting thread";
      }
    }
  };

  static const objpipe_category_t cat;
  return cat;
}

/**
 * \brief Create an \ref objpipe_category() error condition.
 * \ingroup objpipe_errors
 * \param e The error code for which to create an error condition.
 */
inline std::error_condition make_error_condition(objpipe_errc e) {
  return std::error_condition(static_cast<int>(e), objpipe_category());
}

/**
 * \brief Write errc to output stream.
 * \ingroup objpipe_errors
 */
inline std::ostream& operator<<(std::ostream& out, objpipe_errc e) {
  switch (e) {
    case objpipe_errc::success:
      out << "objpipe_errc[success]";
      break;
    case objpipe_errc::closed:
      out << "objpipe_errc[closed]";
      break;
    case objpipe_errc::bad:
      out << "objpipe_errc[bad]";
      break;
    case objpipe_errc::no_thread:
      out << "objpipe_errc[no_thread]";
      break;
  }
  return out;
}

/**
 * \brief Objpipe exception class.
 * \ingroup objpipe_errors
 */
class objpipe_error
: public std::system_error
{
 public:
  ///\brief Constructor.
  ///\param[in] e The error code of the exception.
  objpipe_error(objpipe_errc e)
  : std::system_error(static_cast<int>(e), objpipe_category())
  {}

  ///\brief Constructor.
  ///\param[in] e The error code of the exception.
  ///\param[in] what_arg A text describing what went wrong.
  objpipe_error(objpipe_errc e, const std::string& what_arg)
  : std::system_error(static_cast<int>(e), objpipe_category(), what_arg)
  {}

  ///\brief Constructor.
  ///\param[in] e The error code of the exception.
  ///\param[in] what_arg A text describing what went wrong.
  objpipe_error(objpipe_errc e, const char* what_arg)
  : std::system_error(static_cast<int>(e), objpipe_category(), what_arg)
  {}

  ///\brief Destructor.
  ~objpipe_error() override {};
};


} /* namespace objpipe */

#endif /* OBJPIPE_ERRC_H */
