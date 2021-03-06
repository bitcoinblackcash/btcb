#include <btcb/node/common.hpp>
#include <btcb/node/wallet.hpp>
#include <btcb/secure/blockstore.hpp>

#include <boost/polymorphic_cast.hpp>

btcb::summation_visitor::summation_visitor (btcb::transaction const & transaction_a, btcb::block_store & store_a) :
transaction (transaction_a),
store (store_a)
{
}

void btcb::summation_visitor::send_block (btcb::send_block const & block_a)
{
	assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		sum_set (block_a.hashables.balance.number ());
		current->balance_hash = block_a.hashables.previous;
		current->amount_hash = 0;
	}
	else
	{
		sum_add (block_a.hashables.balance.number ());
		current->balance_hash = 0;
	}
}

void btcb::summation_visitor::state_block (btcb::state_block const & block_a)
{
	assert (current->type != summation_type::invalid && current != nullptr);
	sum_set (block_a.hashables.balance.number ());
	if (current->type == summation_type::amount)
	{
		current->balance_hash = block_a.hashables.previous;
		current->amount_hash = 0;
	}
	else
	{
		current->balance_hash = 0;
	}
}

void btcb::summation_visitor::receive_block (btcb::receive_block const & block_a)
{
	assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		current->amount_hash = block_a.hashables.source;
	}
	else
	{
		btcb::block_info block_info;
		if (!store.block_info_get (transaction, block_a.hash (), block_info))
		{
			sum_add (block_info.balance.number ());
			current->balance_hash = 0;
		}
		else
		{
			current->amount_hash = block_a.hashables.source;
			current->balance_hash = block_a.hashables.previous;
		}
	}
}

void btcb::summation_visitor::open_block (btcb::open_block const & block_a)
{
	assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		if (block_a.hashables.source != btcb::genesis_account)
		{
			current->amount_hash = block_a.hashables.source;
		}
		else
		{
			sum_set (btcb::genesis_amount);
			current->amount_hash = 0;
		}
	}
	else
	{
		current->amount_hash = block_a.hashables.source;
		current->balance_hash = 0;
	}
}

void btcb::summation_visitor::change_block (btcb::change_block const & block_a)
{
	assert (current->type != summation_type::invalid && current != nullptr);
	if (current->type == summation_type::amount)
	{
		sum_set (0);
		current->amount_hash = 0;
	}
	else
	{
		btcb::block_info block_info;
		if (!store.block_info_get (transaction, block_a.hash (), block_info))
		{
			sum_add (block_info.balance.number ());
			current->balance_hash = 0;
		}
		else
		{
			current->balance_hash = block_a.hashables.previous;
		}
	}
}

btcb::summation_visitor::frame btcb::summation_visitor::push (btcb::summation_visitor::summation_type type_a, btcb::block_hash const & hash_a)
{
	frames.emplace (type_a, type_a == summation_type::balance ? hash_a : 0, type_a == summation_type::amount ? hash_a : 0);
	return frames.top ();
}

void btcb::summation_visitor::sum_add (btcb::uint128_t addend_a)
{
	current->sum += addend_a;
	result = current->sum;
}

void btcb::summation_visitor::sum_set (btcb::uint128_t value_a)
{
	current->sum = value_a;
	result = current->sum;
}

btcb::uint128_t btcb::summation_visitor::compute_internal (btcb::summation_visitor::summation_type type_a, btcb::block_hash const & hash_a)
{
	push (type_a, hash_a);

	/*
	 Invocation loop representing balance and amount computations calling each other.
	 This is usually better done by recursion or something like boost::coroutine2, but
	 segmented stacks are not supported on all platforms so we do it manually to avoid
	 stack overflow (the mutual calls are not tail-recursive so we cannot rely on the
	 compiler optimizing that into a loop, though a future alternative is to do a
	 CPS-style implementation to enforce tail calls.)
	*/
	while (frames.size () > 0)
	{
		current = &frames.top ();
		assert (current->type != summation_type::invalid && current != nullptr);

		if (current->type == summation_type::balance)
		{
			if (current->awaiting_result)
			{
				sum_add (current->incoming_result);
				current->awaiting_result = false;
			}

			while (!current->awaiting_result && (!current->balance_hash.is_zero () || !current->amount_hash.is_zero ()))
			{
				if (!current->amount_hash.is_zero ())
				{
					// Compute amount
					current->awaiting_result = true;
					push (summation_type::amount, current->amount_hash);
					current->amount_hash = 0;
				}
				else
				{
					auto block (store.block_get (transaction, current->balance_hash));
					assert (block != nullptr);
					block->visit (*this);
				}
			}

			epilogue ();
		}
		else if (current->type == summation_type::amount)
		{
			if (current->awaiting_result)
			{
				sum_set (current->sum < current->incoming_result ? current->incoming_result - current->sum : current->sum - current->incoming_result);
				current->awaiting_result = false;
			}

			while (!current->awaiting_result && (!current->amount_hash.is_zero () || !current->balance_hash.is_zero ()))
			{
				if (!current->amount_hash.is_zero ())
				{
					auto block (store.block_get (transaction, current->amount_hash));
					if (block != nullptr)
					{
						block->visit (*this);
					}
					else
					{
						if (current->amount_hash == btcb::genesis_account)
						{
							sum_set (std::numeric_limits<btcb::uint128_t>::max ());
							current->amount_hash = 0;
						}
						else
						{
							assert (false);
							sum_set (0);
							current->amount_hash = 0;
						}
					}
				}
				else
				{
					// Compute balance
					current->awaiting_result = true;
					push (summation_type::balance, current->balance_hash);
					current->balance_hash = 0;
				}
			}

			epilogue ();
		}
	}

	return result;
}

void btcb::summation_visitor::epilogue ()
{
	if (!current->awaiting_result)
	{
		frames.pop ();
		if (frames.size () > 0)
		{
			frames.top ().incoming_result = current->sum;
		}
	}
}

btcb::uint128_t btcb::summation_visitor::compute_amount (btcb::block_hash const & block_hash)
{
	return compute_internal (summation_type::amount, block_hash);
}

btcb::uint128_t btcb::summation_visitor::compute_balance (btcb::block_hash const & block_hash)
{
	return compute_internal (summation_type::balance, block_hash);
}

btcb::representative_visitor::representative_visitor (btcb::transaction const & transaction_a, btcb::block_store & store_a) :
transaction (transaction_a),
store (store_a),
result (0)
{
}

void btcb::representative_visitor::compute (btcb::block_hash const & hash_a)
{
	current = hash_a;
	while (result.is_zero ())
	{
		auto block (store.block_get (transaction, current));
		assert (block != nullptr);
		block->visit (*this);
	}
}

void btcb::representative_visitor::send_block (btcb::send_block const & block_a)
{
	current = block_a.previous ();
}

void btcb::representative_visitor::receive_block (btcb::receive_block const & block_a)
{
	current = block_a.previous ();
}

void btcb::representative_visitor::open_block (btcb::open_block const & block_a)
{
	result = block_a.hash ();
}

void btcb::representative_visitor::change_block (btcb::change_block const & block_a)
{
	result = block_a.hash ();
}

void btcb::representative_visitor::state_block (btcb::state_block const & block_a)
{
	result = block_a.hash ();
}
