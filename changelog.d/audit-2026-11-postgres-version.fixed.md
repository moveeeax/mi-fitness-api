PostgreSQL version drift: the README badge now says 18 and the helm
`backup.image` default moved from `postgres:16-alpine` to
`postgres:18-alpine`, matching production (CNPG PostgreSQL 18.3,
`deploy/db/README.md`); a pg_dump older than the server major refuses to dump.
