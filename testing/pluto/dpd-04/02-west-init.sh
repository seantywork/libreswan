/testing/guestbin/swan-prep --hostkeys
ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add west-east
ipsec auto --add west-eastnet
ipsec auto --add westnet-east
# don't auto-revive, instead wait for a trigger
ipsec whack --impair revival
echo "initdone"
