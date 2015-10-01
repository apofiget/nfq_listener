REBAR = `which rebar`

compile:
	@$(REBAR) get-deps
	@$(REBAR) compile

clean:
	@$(REBAR) clean
	@rm -f erl_crash.dump
	@rm priv/nfq_node

node:
	@$(REBAR) generate

doc:
	@$(REBAR) doc
