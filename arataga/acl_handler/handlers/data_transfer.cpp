/*!
 * @file
 * @brief Реализация data-trasfer-handler-а
 */

#include <arataga/acl_handler/connection_handler_ifaces.hpp>
#include <arataga/acl_handler/handler_factories.hpp>
#include <arataga/acl_handler/buffers.hpp>

#include <noexcept_ctcheck/pub.hpp>

namespace arataga::acl_handler
{

namespace handlers::data_transfer
{

//
// data_transfer_handler_t
//
/*!
 * @brief Реализация connection-handler-а для случая, когда
 * соединение уже установлено и нужно только передавать данные
 * туда-обратно.
 *
 * В текущей реализации используется простая схема с единственным
 * входящим буфером по каждому направлению. Сперва из сокета
 * читается N байт во входящий буфер. Затем эти N байт записываются
 * в во второй сокет. Как только запись завершилась, опять читается
 * N байт в этот входящий буфер из первого сокета.
 *
 * Такая же ситуация происходит и со вторым сокетом. Сперва читается
 * N байт во входящий буфер второго сокета. Затем прочитанные данные
 * записываются в первый сокет. После записи вновь возвращаемся к
 * чтению из второго сокета.
 *
 * Т.е. не может быть ситуации, когда мы из сокета вычитали N байт
 * во входящий буфер, инициировали их запись во второй сокет, а
 * сами инициировали следующее чтение из первого сокета (но уже в
 * другой буфер).
 *
 * Соответственно, на ограничение трафика влияет количество данных,
 * прочитанных из того или иного сокета. Так, если данные прочитаны
 * из user-end, то их объем учитывается при ограничениях на исходящий
 * от клиента трафик. А если данные прочитаны из target-end, то их
 * объем учитывается при ограничениях на входящий трафик.
 */
class data_transfer_handler_t final : public connection_handler_t
{
	//! Исходящее соединение.
	asio::ip::tcp::socket m_out_connection;

	//! Ограничитель трафика для этого подключения.
	traffic_limiter_unique_ptr_t m_traffic_limiter;

	//! Размер буферов ввода-вывода.
	/*!
	 * Значение берется из конфигурации при создании объекта
	 * connect_data_transfer_handler_t и затем больше не меняется.
	 */
	const std::size_t m_io_chunk_size;

	//! Описание буферов и состояния одного направления.
	struct direction_state_t
	{
		//! Сокет, который отвечает за это направление.
		asio::ip::tcp::socket & m_channel;

		//! Название этого направления.
		/*!
		 * @attention
		 * Исходим из того, что это string_view для строкового литерала.
		 */
		const std::string_view m_name;

		//! Данные, которые были прочитаны из этого направления и
		//! должны быть переданы в противоположное.
		std::unique_ptr< std::byte[] > m_data_read;
		//! Количество данных, которое находится сейчас в data_read.
		/*!
		 * Обновляется после каждого удачного чтения из m_channel.
		 */
		std::size_t m_data_size{ 0u };

		//! Тип этого направления для учета в traffic_limiter.
		traffic_limiter_t::direction_t m_traffic_direction;

		//! Живо ли еще это направление?
		bool m_is_alive{ true };

		//! Превышен ли лимит трафика по этому направлению?
		bool m_is_traffic_limit_exceeded{ false };

		direction_state_t(
			asio::ip::tcp::socket & channel,
			// Предполагается, что это string_view для строкового литерала.
			std::string_view name,
			std::size_t io_chunk_size,
			traffic_limiter_t::direction_t traffic_direction )
			:	m_channel{ channel }
			,	m_name{ name }
			,	m_data_read{ std::make_unique<std::byte[]>(io_chunk_size) }
			,	m_traffic_direction{ traffic_direction }
		{}
	};

	//! Направление от клиента к удаленному узлу.
	direction_state_t m_user_end;
	//! Направление от удаленного узла к клиенту.
	direction_state_t m_target_end;

	//! Время последнего успешного чтения данных (из любого соединения).
	std::chrono::steady_clock::time_point m_last_read_at{
			std::chrono::steady_clock::now()
		};

	[[nodiscard]]
	static traffic_limiter_unique_ptr_t
	ensure_traffic_limiter_not_null(
		traffic_limiter_unique_ptr_t value )
	{
		if( !value )
			throw acl_handler_ex_t{
					"data_transfer_handler_t's constructor: "
					"traffic_limiter parameter can't be nullptr!"
				};

		return value;
	}

public:
	data_transfer_handler_t(
		handler_context_holder_t ctx,
		handler_context_t::connection_id_t id,
		asio::ip::tcp::socket in_connection,
		asio::ip::tcp::socket out_connection,
		traffic_limiter_unique_ptr_t traffic_limiter )
		:	connection_handler_t{ std::move(ctx), id, std::move(in_connection) }
		,	m_out_connection{ std::move(out_connection) }
		,	m_traffic_limiter{
				ensure_traffic_limiter_not_null( std::move(traffic_limiter) )
			}
		,	m_io_chunk_size{ context().config().io_chunk_size() }
		,	m_user_end{
				m_connection, "user-end", m_io_chunk_size,
				traffic_limiter_t::direction_t::from_user
			}
		,	m_target_end{
				m_out_connection, "target-end", m_io_chunk_size,
				traffic_limiter_t::direction_t::from_target
			}
	{
	}

protected:
	void
	on_start_impl( delete_protector_t delete_protector ) override
	{
		wrap_action_and_handle_exceptions(
			delete_protector,
			[this]( delete_protector_t, can_throw_t can_throw ) {
				// Инициируем чтение из обоих соединений. Какие данные первыми
				// придут, те и пойдут на доставку.
				initiate_read_user_end( can_throw );
				initiate_read_target_end( can_throw );
			} );
	}

	void
	on_timer_impl( delete_protector_t delete_protector ) override
	{
		wrap_action_and_handle_exceptions(
			delete_protector,
			[this]( delete_protector_t delete_protector, can_throw_t can_throw ) {
				// Так быть не должно, но на всякий случай сделаем проверку...
				if( !m_user_end.m_is_alive && !m_target_end.m_is_alive )
				{
					return log_and_remove_connection(
							delete_protector,
							can_throw,
							remove_reason_t::unexpected_and_unsupported_case,
							spdlog::level::warn,
							"both connections are closed" );
				}

				// Какое-то из соединений еще живо. Поэтому можно
				// проверять время отсутствия активности.
				const auto now = std::chrono::steady_clock::now();

				if( m_last_read_at +
						context().config().idle_connection_timeout() < now )
				{
					return log_and_remove_connection(
							delete_protector,
							can_throw,
							remove_reason_t::no_activity_for_too_long,
							spdlog::level::warn,
							"no data read for long time" );
				}

				// Если по каким-то направлениям был превышен лимит,
				// то нужно проверить лимит еще раз, и если можно, то
				// инициировать новое чтение.
				if( m_user_end.m_is_traffic_limit_exceeded )
				{
					// Безопасно инициировать новое чтение, т.к. внутри
					// initiate_* будет еще одна проверка в результате
					// которой флаг будет либо сброшен, либо оставлен
					// как есть.
					initiate_read_user_end( can_throw );
				}
				if( m_target_end.m_is_traffic_limit_exceeded )
				{
					initiate_read_target_end( can_throw );
				}
			} );
	}

	std::string_view
	name() const noexcept override
	{
		return "data-transfer-handler";
	}

	// Нужно переопределить этот метод поскольку появилось еще одно
	// подключение, которое нужно контролировать.
	void
	release() noexcept override
	{
		// Проглатываем возможные ошибки.
		asio::error_code ec;
		m_out_connection.shutdown( asio::ip::tcp::socket::shutdown_both, ec );
		m_out_connection.close( ec );

		// И позволяем очисить ресурсы базовому классу.
		connection_handler_t::release();
	}

private:
	void
	initiate_read_user_end(
		can_throw_t can_throw )
	{
		initiate_async_read_for_direction(
				can_throw, m_user_end, m_target_end );
	}

	void
	initiate_read_target_end(
		can_throw_t can_throw )
	{
		initiate_async_read_for_direction(
				can_throw, m_target_end, m_user_end );
	}

	void
	initiate_async_read_for_direction(
		can_throw_t,
		// Откуда данные нужно читать.
		direction_state_t & src_dir,
		// Куда затем данные нужно записывать.
		direction_state_t & dest_dir )
	{
		// Нужно определить, сколько мы можем прочитать на этом шаге.
		const auto reserved_capacity = m_traffic_limiter->reserve_read_portion(
				src_dir.m_traffic_direction, m_io_chunk_size );

		// Если ничего не можем прочитать, значит превышен лимит.
		src_dir.m_is_traffic_limit_exceeded = ( 0u == reserved_capacity.m_capacity );

		if( src_dir.m_is_traffic_limit_exceeded )
			// Читать данные нельзя, нужно ждать наступления следующего такта.
			return;

		src_dir.m_channel.async_read_some(
				asio::buffer(
						src_dir.m_data_read.get(),
						reserved_capacity.m_capacity),
				with<const asio::error_code &, std::size_t>().make_handler(
					[this, &src_dir, &dest_dir, reserved_capacity](
						delete_protector_t delete_protector,
						can_throw_t can_throw,
						const asio::error_code & ec,
						std::size_t bytes )
					{
						reserved_capacity.release(
								*m_traffic_limiter,
								src_dir.m_traffic_direction,
								ec,
								bytes );

						on_read_result(
								delete_protector,
								can_throw,
								src_dir, dest_dir,
								ec,
								bytes );
					} )
			);
	}

	void
	initiate_async_write_for_direction(
		can_throw_t,
		// Куда должны уйти данные.
		direction_state_t & dest_dir,
		// Откуда данные нужно брать.
		direction_state_t & src_dir )
	{
		// Просим Asio записать все, что у нас есть в буфере.
		asio::async_write(
				dest_dir.m_channel,
				asio::buffer(src_dir.m_data_read.get(), src_dir.m_data_size),
				with<const asio::error_code &, std::size_t>().make_handler(
					[this, &dest_dir, &src_dir](
						delete_protector_t delete_protector,
						can_throw_t can_throw,
						const asio::error_code & ec,
						std::size_t bytes )
					{
						on_write_result(
								delete_protector,
								can_throw,
								dest_dir, src_dir,
								ec, bytes );
					} )
			);
	}

	void
	on_read_result(
		delete_protector_t delete_protector,
		can_throw_t can_throw,
		// Откуда данные были прочитаны.
		direction_state_t & src_dir,
		// Куда данные должны быть записаны.
		direction_state_t & dest_dir,
		const asio::error_code & ec,
		std::size_t bytes_transferred )
	{
		// Этот код оставлен под комментарием для того, чтобы проще было
		// вернуть его при необрходимости отладки.
#if 0
		log( can_throw, spdlog::level::trace,
				fmt::format( "on_read_result {}, ec: {}, bytes: {}",
						src_dir.m_name, ec.message(), bytes_transferred) );
#endif


		// Если это значение по итогу окажется не пустым, значит нужно
		// прекращать работу соединения.
		std::optional< remove_reason_t > remove_reason;

		if( ec )
		{
			src_dir.m_is_alive = false;

			// Суть в том, что операция чтения для src_dir вызывается
			// только тогда, когда все ранее прочитаные данные из src_dir
			// были записаны в dest_dir. Поэтому, если src_dir закрылся,
			// то записывать в dest_dir больше нечего и продолжать работу
			// нельзя.
			//
			// Так что нам остается только выяснить с какой именно
			// диагностикой завершать свою работу.
			if( asio::error::eof == ec )
			{
				remove_reason = remove_reason_t::normal_completion;
			}
			else if( asio::error::operation_aborted == ec )
			{
				remove_reason = remove_reason_t::current_operation_canceled;
			}
			else
			{
				// Возможно, мы наткнулись на ошибку ввода-вывода.
				// Но, может быть просто сейчас завершается наша работа и
				// сокет был закрыт, а Asio выдал код ошибки, отличный
				// от operation_aborted.
				if( src_dir.m_channel.is_open() )
				{
					// Все-таки это ошибка ввода-вывода.
					remove_reason = remove_reason_t::io_error;

					// Залогируем ошибку.
					::arataga::logging::wrap_logging(
							proxy_logging_mode,
							spdlog::level::debug,
							[this, can_throw, &src_dir, &ec]( auto level )
							{
								log_message_for_connection(
										can_throw,
										level,
										fmt::format( "error reading data from {}: {}",
												src_dir.m_name,
												ec.message() ) );
							} );
				}
				else
					remove_reason = remove_reason_t::current_operation_canceled;
			}
		}

		if( remove_reason )
		{
			// Смысла продолжать нет, нужно удалять самих себя.
			remove_handler( delete_protector, *remove_reason );
		}
		else
		{
			// Т.к. нет ошибок, то доверяем значению bytes_transferred.
			src_dir.m_data_size = bytes_transferred;

			// Должны зафиксировать время последней активности.
			m_last_read_at = std::chrono::steady_clock::now();

			// И теперь уже можно отсылать прочитанные данные в другую сторону.
			initiate_async_write_for_direction( can_throw, dest_dir, src_dir );
		}
	}

	void
	on_write_result(
		delete_protector_t delete_protector,
		can_throw_t can_throw,
		// Куда данные были записаны.
		direction_state_t & dest_dir,
		// Откуда данные были прочитаны.
		direction_state_t & src_dir,
		const asio::error_code & ec,
		std::size_t bytes_transferred )
	{
		// При диагностировании ошибок записи просто прекращаем работу.
		if( ec )
		{
			log_and_remove_connection_on_io_error(
					delete_protector,
					can_throw, ec,
					fmt::format( "writting to {}", dest_dir.m_name ) );
		}
		else
		{
			// В принципе, bytes_transferred должен быть равен
			// src_dir.m_data_size. Но если это не так, то продолжать
			// работу нельзя, т.к. это нарушение обещаний, на которые
			// мы расчитываем.
			if( src_dir.m_data_size != bytes_transferred )
			{
				log_and_remove_connection(
						delete_protector,
						can_throw,
						remove_reason_t::io_error,
						spdlog::level::critical,
						fmt::format( "unexpected write result: {} data_size {} != "
								"bytes_transferred {}",
								dest_dir.m_name,
								src_dir.m_data_size,
								bytes_transferred ) );
			}
			else
			{
				// Т.к. мы записали очередную порцию данных
				// от клиента к целевому узлу, то нужно инициировать
				// чтение очередной порции данных.
				initiate_async_read_for_direction( can_throw, src_dir, dest_dir );
			}
		}
	}
};

} /* namespace handlers::data_transfer */

//
// make_data_transfer_handler
//
[[nodiscard]]
connection_handler_shptr_t
make_data_transfer_handler(
	handler_context_holder_t ctx,
	handler_context_t::connection_id_t id,
	asio::ip::tcp::socket in_connection,
	asio::ip::tcp::socket out_connection,
	traffic_limiter_unique_ptr_t traffic_limiter )
{
	using namespace handlers::data_transfer;

	return std::make_shared< data_transfer_handler_t >(
			std::move(ctx), id,
			std::move(in_connection),
			std::move(out_connection),
			std::move(traffic_limiter) );
}

} /* namespace arataga::acl_handler */

