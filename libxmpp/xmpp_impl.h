/*
 * Copyright (C) 2012  微蔡 <microcai@fedoraproject.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef __XMPP_IMPL_H
#define __XMPP_IMPL_H

#include <string>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <gloox/logsink.h>
#include <gloox/client.h>
#include <gloox/messagehandler.h>
#include <gloox/connectionlistener.h>
#include <gloox/mucroomhandler.h>
#include <gloox/connectionbase.h>

#include <boost/signal.hpp>

namespace XMPP {

class xmpp_asio_connector : public gloox::ConnectionBase
{
public:
	xmpp_asio_connector(boost::asio::io_service & , gloox::ConnectionDataHandler* cdh, boost::asio::ip::tcp::resolver::query _query);
private: // for gloox::ConnectionTCPClient
    virtual bool send(const std::string& data);
    virtual gloox::ConnectionError connect();
    virtual void disconnect();
    virtual void getStatistics(long int& totalIn, long int& totalOut){}
    virtual gloox::ConnectionError receive();
    virtual gloox::ConnectionError recv(int timeout = -1);
    virtual ConnectionBase* newInstance() const
    {
		return new xmpp_asio_connector(io_service, m_handler, m_query);
	}
private: // for asio callbacks
	void cb_handle_connecting(const boost::system::error_code & ec);
	void cb_handle_asio_read(const boost::system::error_code & error, std::size_t bytes_transferred);
	void cb_handle_asio_write(const boost::system::error_code & error, std::size_t bytes_transferred);

private:
	boost::asio::io_service	&io_service;
	boost::asio::ip::tcp::socket m_socket;
	boost::asio::ip::tcp::resolver::query m_query;
	boost::array<char, 8192>	m_readbuf;
};

class xmpp_impl : private gloox::MessageHandler, gloox::ConnectionListener, gloox::MUCRoomHandler
{
public:
	xmpp_impl(boost::asio::io_service & asio, std::string xmppuser, std::string xmpppasswd, std::string xmppserver, std::string xmppnick);
	void join(std::string roomjid);
	void on_room_message(boost::function<void (std::string xmpproom, std::string who, std::string message)> cb);
	void send_room_message(std::string xmpproom, std::string message);
private:
	void start();

private:  // for ConnectionListener
    virtual void handleMessage(const gloox::Message& msg, gloox::MessageSession* session = 0);

    virtual void onConnect();
    virtual void onDisconnect(gloox::ConnectionError e);
    virtual bool onTLSConnect(const gloox::CertInfo& info);

    virtual void handleMUCMessage(gloox::MUCRoom* room, const gloox::Message& msg, bool priv);
    virtual void handleMUCParticipantPresence(gloox::MUCRoom* room, const gloox::MUCRoomParticipant participant, const gloox::Presence& presence);
    virtual void handleMUCSubject(gloox::MUCRoom* room, const std::string& nick, const std::string& subject);
    virtual void handleMUCError(gloox::MUCRoom* room, gloox::StanzaError error);
    virtual void handleMUCInfo(gloox::MUCRoom* room, int features, const std::string& name, const gloox::DataForm* infoForm);
    virtual void handleMUCInviteDecline(gloox::MUCRoom* room, const gloox::JID& invitee, const std::string& reason);

    virtual void handleMUCItems(gloox::MUCRoom* room, const gloox::Disco::ItemList& items);
    virtual bool handleMUCRoomCreation(gloox::MUCRoom* room);

private:
	boost::asio::io_service & io_service;
	gloox::JID m_jid;
	gloox::Client m_client;
	std::string		m_xmppnick;
	std::vector<boost::shared_ptr<gloox::MUCRoom> >	m_rooms;
	boost::asio::streambuf							m_readbuf;
	
	boost::signal <void (std::string xmpproom, std::string who, std::string message)> m_sig_room_message;
};

}
#endif // __XMPP_IMPL_H

