# Moving the database to the new server

The PostgreSQL database moves from the old machine in the basement to the new
rack server on Saturday, 7 November. The service is offline from 22:00 to 02:00;
customers were told by email two weeks before. Anna runs the migration, Jonas is
on call.

The data is copied with pg_dump and restored on the new server, then the row
counts of the twenty largest tables are compared. Only when they match does the
DNS entry for db.internal switch to the new address.

If anything goes wrong, the old server stays untouched until Monday. Rolling
back means pointing the DNS entry at the old address again; data written after
22:00 on the new server would be lost, which is why the service is offline.
