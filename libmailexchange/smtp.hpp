#pragma once

#include <string>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/format.hpp>
#include <boost/foreach.hpp>
#include "boost/coro/coro.hpp"
#include "boost/coro/yield.hpp"
#include "boost/timedcall.hpp"
#include "avproxy.hpp"

#include "internet_mail_format.hpp"

class check_smtp_response{
public:
	template<class streambuf>
	check_smtp_response(streambuf * _readbuf, boost::system::error_code & ec, unsigned  response_code, boost::function<void (std::string)> callback = boost::function<void (std::string)>())
	{
		using namespace boost::system::errc;
		using namespace boost::system;
		// 如果出现了 response_code 以外的信息, 那可不正确哦!
		std::istream	stream(_readbuf);
		while (!stream.eof())
		{
			std::string 	line;
			std::getline(stream, line);
			boost::trim_right(line);
			boost::cmatch what;

			std::cout <<  line <<  std::endl;
			std::string ex1 = boost::str(boost::format("%d (.*)?") % response_code);
			std::string ex2 = boost::str(boost::format("%d-(.*)?") % response_code);
			if (line.empty())
				continue;
			if (boost::regex_match(line.c_str(), what, boost::regex(ex1)))
			{
				if (callback)
					callback(what[1]);
				// 成功
				ec = make_error_code(errc::success);
				return ;
			}else if (boost::regex_match(line.c_str(), what, boost::regex(ex2))){
				if (callback)
					callback(what[1]);
				continue; 
			}// 没读到 "response_code-"
			break;
		}
		ec = make_error_code(protocol_error);
	}
};

// ----------------

// 发送 rcpt to 命令, 返回发送成功的个数. 如果没有一个成功, 返回错误.
class send_rcpt_tos : boost::coro::coroutine {
public:
	typedef boost::asio::ip::tcp::socket	socket_type;

	template<class Handler>
	send_rcpt_tos(socket_type & socket, boost::shared_ptr<boost::asio::streambuf> _readbuf, 
	const std::vector<std::string> & _rcpts, 
	Handler handler)
	  : m_socket(socket), m_readbuf(_readbuf), m_rcpts(_rcpts), m_handler(handler)
	{
		socket.get_io_service().post(boost::asio::detail::bind_handler(*this, boost::system::error_code(), 0));
	}

	void operator()(boost::system::error_code ec, std::size_t bytetransfered)
	{
   		using namespace boost::asio;

		reenter(this)
		{
			using namespace boost::system::errc;
			for (sended_rcpt=i=0;i<m_rcpts.size();i++){
				// 开始发送循环.
				_yield m_socket.async_write_some(buffer(m_rcpts[i]), *this);
				if (ec)
					break;
				// 读取响应.
 				_yield read_smtp_response_lines(*this);
 				if (ec)
					break;
				// 检查是不是 250 OK
				check_smtp_response(m_readbuf.get(), ec, 250);
				if (!ec) // 是 250 就加 1
				{	sended_rcpt ++;}
				ec.clear();
			}

			if (sended_rcpt ==0){
				ec = make_error_code(permission_denied);
			}
			m_socket.get_io_service().post(boost::asio::detail::bind_handler(m_handler, ec, sended_rcpt));
		}
	}

private:
	template<class Handler>
	void read_smtp_response_lines(Handler handler)
	{
		boost::asio::async_read_until(m_socket, *m_readbuf, boost::regex("[0-9]*? .*?\n"), handler);
	}

private:
	boost::function<void (const boost::system::error_code & ec, std::size_t count) >	m_handler;
	socket_type& m_socket;
	boost::shared_ptr<boost::asio::streambuf> m_readbuf;
	std::vector<std::string> m_rcpts;
	int i;
	int sended_rcpt;
};

class smtp {
	boost::asio::io_service & io_service;
	std::string m_mailaddr,m_passwd,m_mailserver;
	std::string m_AUTH;
	InternetMailFormat m_imf;

	// 必须是可拷贝的，所以只能用共享指针.
	boost::shared_ptr<boost::asio::ip::tcp::socket> m_socket;
	boost::shared_ptr<boost::asio::streambuf> m_readbuf;

	std::vector<std::string> rcpts;// 接收的人

	// for operator()()
	int retry_count;

public:
	smtp(::boost::asio::io_service & _io_service, std::string user, std::string passwd, std::string _mailserver="");

	// ---------------------------------------------

	// 使用 async_sendmail 异步发送一份邮件.邮件的内容由 InternetMailFormat 指定.
	// 请不要对 InternetMailFormat 执行 base64 编码, async_sendmail 发现里面包含了
	// 非ASCII编码的时候, 就会对内容执行base64编码.
	// 所以也请不要自行设置 content-transfer-encoding
	template<class Handler>
	void async_sendmail(const InternetMailFormat &imf, Handler handler)
	{
		retry_count = 0;
		// 依据 imf.header["to"] 拆分
		std::vector<std::string>	mails;

		detail::mail_address_split(mails, imf.header.at(std::string("to")));

		BOOST_FOREACH(std::string rcpt, mails)
		{
			boost::cmatch	what;
			boost::regex	ex("[A-Za-z0-9\\._\\-]*@[A-Za-z0-9\\._\\-]*");
			// 有的是 u@d 的形式,  有的是 "name" <u@d> 的形式呢
			if (boost::regex_search(rcpt.c_str(), what, ex))
			{
				std::string mailaddress = what[0];
				std::string rcpt_command = boost::str(boost::format("rcpt to %s\r\n") % mailaddress);
				rcpts.push_back(rcpt_command);
			}
		}
		m_imf = imf;
 		io_service.post(boost::bind(*this, boost::system::error_code(), 0, handler, boost::coro::coroutine()));
	}

	template<class Handler>
	void operator()(boost::system::error_code ec, std::size_t bytetransfered, Handler handler, boost::coro::coroutine coro)
	{
 		using namespace boost::asio;

 		std::string		status;
		std::string		maillength;
		std::string		msg;

 		// 在这里就使用 goto failed, 就不用在内部每次执行判定了, 轻松愉快.
		if (ec){
			// 要重试不? 
   			if (retry_count++ <= 20)
			{
				// 如果失败, 等待 retry_count * 10 + 20 s 后, 重试
				::boost::delayedcallsec( io_service, retry_count * 10 + 20, boost::bind(*this, boost::system::error_code(), 0, handler, boost::coro::coroutine()) );
				return ;
			}else{
	   			// 重试次数到达极限.
				// 执行失败通知
				io_service.post(boost::asio::detail::bind_handler(handler, ec));
				return ;
			}
		}

		reenter(&coro)
		{
			m_socket.reset( new ip::tcp::socket(io_service));

			// 首先链接到服务器. dns 解析并连接.
			_yield avproxy::async_proxy_connect(
					avproxy::autoproxychain(*m_socket, ip::tcp::resolver::query(m_mailserver, "25")),
					boost::bind(*this, _1, 0, handler, coro));

			m_readbuf.reset(new boost::asio::streambuf);

			// 读取欢迎信息. 以 220 XXX 终止.如果多行, 最后一个是 带空格的, 前面的都不带. 故而使用 "[0-9]*? .*?\n" 正则表达式
			_yield read_smtp_response_lines(boost::bind(*this, _1, _2, handler, coro));
			// discard 220 message
			_yield check_wellcome_msg(boost::bind(*this, _1, 0, handler, coro));

			// 发送 EHLO 执行登录.
			_yield m_socket->async_write_some( buffer( std::string ( "EHLO " ) + m_mailserver  + "\r\n" ), boost::bind(*this, _1, _2, handler, coro) );
			_yield read_smtp_response_lines(boost::bind(*this, _1, _2, handler, coro));

			// 读取 250 响应.
			_yield check_server_cap(boost::bind(*this, _1, 0, handler, coro));

			// 发送 AUTH PLAIN XXX 登录认证.
			_yield m_socket->async_write_some( buffer( std::string ( "AUTH PLAIN " ) + m_AUTH  + "\r\n" ), boost::bind(*this, _1, _2, handler, coro) );
			_yield read_smtp_response_lines(boost::bind(*this, _1, _2, handler, coro));

			// 读取 235 响应.
			_yield check_login_status(boost::bind(*this, _1, 0, handler, coro));

			// 进入邮件发送过程.
			// 发送 mail from <>
			_yield m_socket->async_write_some( buffer( std::string ( "MAIL FROM " ) + m_mailaddr + "\r\n" ), boost::bind(*this, _1, _2, handler, coro) );
			_yield read_smtp_response_lines(boost::bind(*this, _1, _2, handler, coro));
			// 检查 OK 返回值
			_yield check_status_for_ok(boost::bind(*this, _1, 0, handler, coro));

			// 发送 rcpt to <>
			_yield send_rcpt_tos(*m_socket, m_readbuf, rcpts, boost::bind(*this, _1, _2, handler, coro));

			// 发送 DATA
			_yield m_socket->async_write_some(buffer(std::string("DATA\r\n")), boost::bind(*this, _1, _2, handler, coro) );
			_yield read_smtp_response_lines(boost::bind(*this, _1, _2, handler, coro));
			// 检查 OK 返回值
			_yield check_status_for(354, boost::bind(*this, _1, 0, handler, coro));
			
			// 发送 IMF 格式化后的数据.
			_yield send_mail_data(boost::bind(*this, _1, _2, handler, coro));

			_yield read_smtp_response_lines(boost::bind(*this, _1, _2, handler, coro));
			// 检查 OK 返回值
			_yield check_status_for_ok(boost::bind(*this, _1, 0, handler, coro));
			// 执行一起正常的回调.
			io_service.post<>(boost::asio::detail::bind_handler(handler, ec));
		}
	}
	typedef void result_type;
private:
	// ----------------------

	// 读取 SMTP 应答. 以 数字[空格]消息 
	// 为终止条件
	template<class Handler>
	void read_smtp_response_lines(Handler handler)
	{
		boost::asio::async_read_until(*m_socket, *m_readbuf, boost::regex("[0-9]*? .*?\n"), handler);
	}

	void  check_smtp_response(boost::system::error_code & ec, unsigned  response_code, boost::function<void (std::string)> callback = boost::function<void (std::string)>())
	{
		::check_smtp_response(m_readbuf.get(), ec, response_code, callback);
	}
	// ---------------------
	
	// 要保证服务器发回的是 250, ok
	template<class Handler>
	void check_status_for_ok(Handler handler)
	{
		check_status_for(250, handler);
	}

	// 要保证服务器发回的是 status_code, ok
	template<class Handler>
	void check_status_for(unsigned status_code, Handler handler)
	{
		boost::system::error_code ec;
		check_smtp_response(ec, status_code);
		io_service.post(boost::asio::detail::bind_handler(handler, ec));
	}

	template<class Handler>
	void check_wellcome_msg(Handler handler)
	{
		boost::system::error_code ec;
		check_smtp_response(ec, 220);
		io_service.post(boost::asio::detail::bind_handler(handler, ec));
	}

	// ---------------------------------
	
	// 检查服务器的能力!
	// 以 250 起始终.
	template<class Handler>
	void check_server_cap(Handler handler)
	{
 		boost::system::error_code ec;
		check_smtp_response(ec, 250);
		io_service.post(boost::asio::detail::bind_handler(handler, ec));
	}

	template<class Handler>
	void check_login_status(Handler handler)
	{
 		boost::system::error_code ec;
		check_smtp_response(ec, 235);
		io_service.post(boost::asio::detail::bind_handler(handler, ec));
	}
	
	template<class Handler>
	void send_mail_data(Handler handler)
	{
		boost::asio::streambuf data;
		std::ostream os(&data);
 		boost::system::error_code ec;
		imf_write_stream(m_imf, os);
		os <<  "\r\n.\r\n";
		boost::asio::async_write(*m_socket, data.data(), handler);
	}
};

