/testing/guestbin/swan-prep --nokeys
/testing/x509/import.sh real/mainca/`hostname`.all.p12
ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add san
echo "initdone"
