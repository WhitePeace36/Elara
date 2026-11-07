# $1 = 1: new installation, >= 2: upgrade
if [ "$1" -eq 1 ]; then
    /usr/lib/elara/elara-hook install
else
    /usr/lib/elara/elara-hook upgrade
fi
