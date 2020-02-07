// file      : mod/database.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <mod/database.hxx>

#include <map>

#include <odb/pgsql/database.hxx>
#include <odb/pgsql/connection-factory.hxx>

namespace brep
{
  struct db_key
  {
    string user;
    string role;
    string password;
    string name;
    string host;
    uint16_t port;
  };

  static bool
  operator< (const db_key& x, const db_key& y)
  {
    int r;
    if ((r = x.user.compare (y.user)) != 0 ||
        (r = x.role.compare (y.role)) != 0 ||
        (r = x.password.compare (y.password)) != 0 ||
        (r = x.name.compare (y.name)) != 0 ||
        (r = x.host.compare (y.host)))
      return r < 0;

    return x.port < y.port;
  }

  using namespace odb;

  class connection_pool_factory: public pgsql::connection_pool_factory
  {
  public:
    connection_pool_factory (string role, size_t max_connections)
        : pgsql::connection_pool_factory (max_connections),
          role_ (move (role))
    {
    }

    virtual pooled_connection_ptr
    create () override
    {
      pooled_connection_ptr conn (pgsql::connection_pool_factory::create ());

      // Set the serializable isolation level for the subsequent connection
      // transactions. Note that the SET TRANSACTION command affects only the
      // current transaction.
      //
      conn->execute ("SET default_transaction_isolation=serializable");

      // Change the connection current user to the execution user name.
      //
      if (!role_.empty ())
        conn->execute ("SET ROLE '" + role_ + "'");

      return conn;
    }

  private:
    string role_;
  };

  shared_ptr<database>
  shared_database (string user,
                   string role,
                   string password,
                   string name,
                   string host,
                   uint16_t port,
                   size_t max_connections)
  {
    static std::map<db_key, weak_ptr<database>> databases;

    db_key k ({
      move (user), move (role), move (password),
      move (name),
      move (host), port});

    auto i (databases.find (k));
    if (i != databases.end ())
    {
      if (shared_ptr<database> d = i->second.lock ())
        return d;
    }

    unique_ptr<pgsql::connection_factory>
      f (new connection_pool_factory (k.role, max_connections));

    shared_ptr<database> d (
      make_shared<pgsql::database> (
        k.user,
        k.password,
        k.name,
        k.host,
        k.port,
        "",
        move (f)));

    databases[move (k)] = d;
    return d;
  }
}
