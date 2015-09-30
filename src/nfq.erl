-module(nfq).

-author("Andrey Andruschenko <apofiget@gmail.com>").

-export([create_queue/2, destroy_queue/2,
         set_mode/3, set_queue_len/3,
         read_pkt_start/2, get_pkt/2,
         read_pkt_stop/2, queue_list/1]).

create_queue(Node, Q) when is_integer(Q) ->
    {cnode, Node} ! {self(), {create_queue, Q}},
    got_reply().

destroy_queue(Node, Q) when is_integer(Q) ->
    {cnode, Node} ! {self(), {destroy_queue, Q}},
    got_reply().

set_mode(Node, Q, Mode) when is_integer(Q), is_integer(Mode) ->
    {cnode, Node} ! {self(), {set_mode, Mode, Q}},
    got_reply().

set_queue_len(Node, Q, Len) when is_integer(Q), is_integer(Len) ->
    {cnode, Node} ! {self(), {set_queue_len, Len, Q}},
    got_reply().

queue_list(Node) ->
    {cnode, Node} ! {self(), {queue_list, ok}},
    got_reply().

read_pkt_start(Node, Q) ->
    {cnode, Node} ! {self(), {read_pkt_start, Q}},
    got_reply().

read_pkt_stop(Node, Q) ->
    {cnode, Node} ! {self(), {read_pkt_stop, Q}},
    got_reply().

get_pkt(Node, _Q) ->
    ok.

got_reply() ->
    receive
        {cnode,{reply, {ok, ok}}} ->
            ok;
        {cnode,{reply, Reply}} ->
            Reply;
        Any ->
            {error, Any}
    after 1000 ->
            {error, timeout}
    end.
