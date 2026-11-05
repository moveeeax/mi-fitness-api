vcpkg builtin-baseline and the Dockerfile `VCPKG_REF` moved together from
c4aad28 (2026-05-01) to microsoft/vcpkg master 5d68747 (2026-11-04):
drogon 1.9.12 to 1.9.14, opentelemetry-cpp 1.26.0 to 1.30.0, libpqxx 8.0.1
to 8.0.2, gtest 1.17.0 to 1.18.0. The `vcpkg.json` description no longer
names the template; its `version` stays frozen at 1.4.0 on purpose.
