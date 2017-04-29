// file      : mod/database.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2017 Code Synthesis Ltd
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
        (r = x.password.compare (y.password)) != 0 ||
        (r = x.name.compare (y.name)) != 0 ||
        (r = x.host.compare (y.host)))
      return r < 0;

    return x.port < y.port;
  }

  using namespace odb;

  shared_ptr<database>
  shared_database (string user,
                   string password,
                   string name,
                   string host,
                   uint16_t port,
                   size_t max_connections)
  {
    static std::map<db_key, weak_ptr<database>> databases;

    db_key k ({move (user), move (password), move (name), host, port});

    auto i (databases.find (k));
    if (i != databases.end ())
    {
      if (shared_ptr<database> d = i->second.lock ())
        return d;
    }

    unique_ptr<pgsql::connection_factory>
      f (new pgsql::connection_pool_factory (max_connections));

    shared_ptr<database> d (
      make_shared<pgsql::database> (
        k.user,
        k.password,
        k.name,
        k.host,
        k.port,
        "options='-c default_transaction_isolation=serializable'",
        move (f)));

    databases[move (k)] = d;
    return d;
  }
}
