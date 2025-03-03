/testing/guestbin/swan-prep --nokeys

/testing/x509/import.sh real/mainca/west-nosan.all.p12
/testing/x509/import.sh real/mainca/east.end.cert

ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add ikev2-westnet-eastnet-x509-cr
ipsec status | grep idtype
ipsec whack --impair suppress_retransmits
echo "initdone"
