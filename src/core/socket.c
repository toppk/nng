//
// Copyright 2016 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

// Socket implementation.

// nni_socket_sendq and nni_socket_recvq are called by the protocol to obtain
// the upper read and write queues.
nni_msgqueue *
nni_socket_sendq(nni_socket *s)
{
	return (s->s_uwq);
}


nni_msgqueue *
nni_socket_recvq(nni_socket *s)
{
	return (s->s_urq);
}


// nn_socket_create creates the underlying socket.
int
nni_socket_create(nni_socket **sockp, uint16_t proto)
{
	nni_socket *sock;
	nni_protocol *ops;
	int rv;

	if ((ops = nni_protocol_find(proto)) == NULL) {
		return (NNG_ENOTSUP);
	}
	if ((sock = nni_alloc(sizeof (*sock))) == NULL) {
		return (NNG_ENOMEM);
	}
	sock->s_ops = *ops;

	if ((rv = nni_mutex_init(&sock->s_mx)) != 0) {
		nni_free(sock, sizeof (*sock));
		return (rv);
	}
	if ((rv = nni_cond_init(&sock->s_cv, &sock->s_mx)) != 0) {
		nni_mutex_fini(&sock->s_mx);
		nni_free(sock, sizeof (*sock));
		return (rv);
	}

	NNI_LIST_INIT(&sock->s_pipes, nni_pipe, p_sock_node);
	NNI_LIST_INIT(&sock->s_eps, nni_endpt, ep_sock_node);

	if ((rv = sock->s_ops.proto_create(&sock->s_data, sock)) != 0) {
		nni_cond_fini(&sock->s_cv);
		nni_mutex_fini(&sock->s_mx);
		nni_free(sock, sizeof (*sock));
		return (rv);
	}
	*sockp = sock;
	return (0);
}


// nni_socket_close closes the underlying socket.
int
nni_socket_close(nni_socket *sock)
{
	nni_pipe *pipe;
	nni_endpt *ep;
	uint64_t linger;


	nni_mutex_enter(&sock->s_mx);
	// Mark us closing, so no more EPs or changes can occur.
	sock->s_closing = 1;

	// Stop all EPS.  We're going to do this first, since we know
	// we're closing.
	NNI_LIST_FOREACH (&sock->s_eps, ep) {
		nni_endpt_close(ep);
	}
	nni_mutex_exit(&sock->s_mx);

	// XXX: TODO.  This is a place where we should drain the write side
	// msgqueue, effectively getting a linger on the socket.  The
	// protocols will drain this queue, and should continue to run
	// handling both transmit and receive until that's done.
	// Note that *all* protocols need to monitor for this linger, even
	// those that do not transmit.  This way they can notice and go ahead
	// and quickly shutdown their pipes; this keeps us from having to wait
	// for *our* pipelist to become empty.  This helps us ensure that
	// they effectively get the chance to linger on their side, without
	// requring us to do anything magical for them.

	// nni_msgqueue_drain(sock->s_uwq, sock->s_linger);

	// Now we should attempt to wait for the list of pipes to drop to
	// zero -- indicating that the protocol has shut things down
	// cleanly, voluntarily.  (I.e. it finished its drain.)
	linger = nni_clock();
	nni_mutex_enter(&sock->s_mx);
	while (nni_list_first(&sock->s_pipes) != NULL) {
		if (nni_cond_waituntil(&sock->s_cv, linger) == NNG_ETIMEDOUT) {
			break;
		}
	}

	// Time's up!  Shut it down the hard way.
	nni_msgqueue_close(sock->s_urq);
	nni_msgqueue_close(sock->s_uwq);
	// This gives the protocol notice, in case it didn't get the hint.
	// XXX: In retrospect, this entry point seems redundant.
	sock->s_ops.proto_shutdown(sock->s_data);

	// The protocol *MUST* give us back our pipes at this point, and
	// quickly too!  If this blocks for any non-trivial amount of time
	// here, it indicates a protocol implementation bug.
	while (nni_list_first(&sock->s_pipes) != NULL) {
		nni_cond_wait(&sock->s_cv);
	}

	// Wait to make sure endpoint listeners have shutdown and exited
	// as well.  They should have done so *long* ago.  Because this
	// requires waiting for threads to finish, which *could* in theory
	// overlap with this, we must drop the socket lock.
	while ((ep = nni_list_first(&sock->s_eps)) != NULL) {

		// TODO: This looks like it should happen as an endpt_remove
		// operation?
		nni_list_remove(&sock->s_eps, ep);
		nni_mutex_exit(&sock->s_mx);

		nni_endpt_destroy(ep);
		nni_mutex_enter(&sock->s_mx);
	}

	nni_mutex_exit(&sock->s_mx);

	return (0);
}


int
nni_socket_sendmsg(nni_socket *sock, nni_msg *msg, nni_duration tmout)
{
	int rv;
	int besteffort;
	nni_time expire;

	if (tmout > 0) {
		expire = nni_clock() + tmout;
	} else if (tmout < 0) {
		expire = NNI_TIME_NEVER;
	} else {
		expire = NNI_TIME_ZERO;
	}

	// Senderr is typically set by protocols when the state machine
	// indicates that it is no longer valid to send a message.  E.g.
	// a REP socket with no REQ pending.
	nni_mutex_enter(&sock->s_mx);
	if (sock->s_closing) {
		nni_mutex_exit(&sock->s_mx);
		return (NNG_ECLOSED);
	}
	if ((rv = sock->s_senderr) != 0) {
		nni_mutex_exit(&sock->s_mx);
		return (rv);
	}
	besteffort = sock->s_besteffort;
	nni_mutex_exit(&sock->s_mx);

	if (sock->s_ops.proto_send_filter != NULL) {
		msg = sock->s_ops.proto_send_filter(sock->s_data, msg);
		if (msg == NULL) {
			return (0);
		}
	}

	if (besteffort) {
		// BestEffort mode -- if we cannot handle the message due to
		// backpressure, we just throw it away, and don't complain.
		expire = NNI_TIME_ZERO;
	}
	rv = nni_msgqueue_put_until(sock->s_uwq, msg, expire);
	if (besteffort && (rv == NNG_EAGAIN)) {
		// Pretend this worked... it didn't, but pretend.
		nni_msg_free(msg);
		return (0);
	}
	return (rv);
}

int
nni_socket_recvmsg(nni_socket *sock, nni_msg **msgp, nni_duration tmout)
{
	int rv;
	nni_time expire;
	nni_msg *msg;

	if (tmout > 0) {
		expire = nni_clock() + tmout;
	} else if (tmout < 0) {
		expire = NNI_TIME_NEVER;
	} else {
		expire = NNI_TIME_ZERO;
	}

	nni_mutex_enter(&sock->s_mx);
	if (sock->s_closing) {
		nni_mutex_exit(&sock->s_mx);
		return (NNG_ECLOSED);
	}
	if ((rv = sock->s_recverr) != 0) {
		nni_mutex_exit(&sock->s_mx);
		return (rv);
	}
	nni_mutex_exit(&sock->s_mx);

	for (;;) {
		rv = nni_msgqueue_get_until(sock->s_urq, &msg, expire);
		if (rv != 0) {
			return (rv);
		}
		msg = sock->s_ops.proto_recv_filter(sock->s_data, msg);
		if (msg != NULL) {
			break;
		}
		// Protocol dropped the message; try again.
	}

	*msgp = msg;
	return (0);
}

// nni_socket_protocol returns the socket's 16-bit protocol number.
uint16_t
nni_socket_proto(nni_socket *sock)
{
	return (sock->s_ops.proto_self);
}

// nni_socket_rem_pipe removes the pipe from the socket.  This is often
// called by the protocol when a pipe is removed due to close.
void
nni_socket_rem_pipe(nni_socket *sock, nni_pipe *pipe)
{
	nni_mutex_enter(&sock->s_mx);
	if (pipe->p_sock != sock) {
		nni_mutex_exit(&sock->s_mx);
	}

	// Remove the pipe from the protocol.  Protocols may
	// keep lists of pipes for managing their topologies.
	sock->s_ops.proto_rem_pipe(sock->s_data, pipe);

	// Now remove it from our own list.
	nni_list_remove(&sock->s_pipes, pipe);
	pipe->p_sock = NULL;
	// XXX: TODO: Redial
	// XXX: also publish event...
	// if (pipe->p_ep != NULL) {
	//	nn_endpt_rem_pipe(pipe->p_ep, pipe)
	// }

	nni_pipe_destroy(pipe);

	// If we're closing, wake the socket if we finished draining.
	if (sock->s_closing && (nni_list_first(&sock->s_pipes) == NULL)) {
		nni_cond_broadcast(&sock->s_cv);
	}
	nni_mutex_exit(&sock->s_mx);
}


int
nni_socket_add_pipe(nni_socket *sock, nni_pipe *pipe)
{
	int rv;

	nni_mutex_enter(&sock->s_mx);
	if (sock->s_closing) {
		nni_mutex_exit(&sock->s_mx);
		return (NNG_ECLOSED);
	}
	if ((rv = sock->s_ops.proto_add_pipe(sock->s_data, pipe)) != 0) {
		nni_mutex_exit(&sock->s_mx);
		return (rv);
	}
	nni_list_append(&sock->s_pipes, pipe);
	pipe->p_sock = sock;
	// XXX: Publish event
	nni_mutex_exit(&sock->s_mx);
	return (0);
}