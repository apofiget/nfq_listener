#!/bin/sh

#export DISPLAY=localhost:10.0

export DISPLAY=:0.0

ulimit -n 4096

#./priv/nfq_node 6543 COOKIE

erl +pc unicode -pa ebin/ deps/lager/ebin/ deps/goldrush/ebin/ -config app.config -sname nf -setcookie COOKIE
