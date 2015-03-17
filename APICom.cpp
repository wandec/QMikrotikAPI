#include "APICom.h"

using namespace Mkt;

APICom::APICom(QObject *papi)
 : QObject(papi), m_loginState(NoLoged), incomingWordSize(-1)
{
	connect( &m_sock, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(onError(QAbstractSocket::SocketError)) );
    connect( &m_sock, SIGNAL(hostFound()), this, SLOT(onHostLookup()) );
	connect( &m_sock, SIGNAL(connected()), this, SLOT(onConnected()) );

	connect( &m_sock, SIGNAL(readyRead()), this, SLOT(onReadyRead()) );
	connect( &m_sock, SIGNAL(disconnected()), this, SLOT(onDisconnected()) );
	connect( &m_sock, SIGNAL(readyRead()), this, SLOT(onReadyRead()) );
}

APICom::~APICom()
{
	m_sock.close();
}

void APICom::writeSentence(QSentence &writeSentence)
{
	if( writeSentence.count() )
	{
		for( int i = 0; i < writeSentence.count(); i++ )
			writeWord(writeSentence[i]);
		writeWord("\0");
	}
}

/********************************************************************
 * Read a word from the socket
 * The word that was read is returned as a string
 ********************************************************************/
// TODO: Esta función no tiene en cuenta que el socket no tenga
// todos los datos disponibles según el tamaño que deba leer.
// Quizá no importe pero esto debería tenerse en cuenta.
int APICom::readWord()
{
	if( incomingWordSize <= 0 )
		if( (incomingWordSize = readLength()) == 0 )
		{
			incomingWord.clear();
			return 0;
		}

	QByteArray tmp = m_sock.read(incomingWordSize);
	if( tmp.count() )
	{
		incomingWordSize -= tmp.count();
		incomingWord.append(tmp);
	}
	return incomingWordSize == 0 ? 1 : -1;
}

/********************************************************************
 * Read a Sentence from the socket
 * A Sentence struct is returned
 ********************************************************************/
void APICom::readSentence()
{
	int i;
	while( (i = readWord()) != 0 )
	{
		switch( i )
		{
		case 1:
			incomingSentence.append(incomingWord);
			incomingWord.clear();
			incomingWordSize = -1;
			break;
		case -1:
			// Aun no ha sacado del todo la palabra. Esperaremos al siguiente evento del socket.
			incomingSentence.setReturnType(QSentence::Partial);
			return;
		}
	}
	// Ha llegado la cadena vacía. Vamos a procesar lo que nos ha llegado.

	// check to see if we can get a return value from the API
	if( incomingSentence.count() )
	{
		if( incomingSentence.first().startsWith("!done") )
		{
			incomingSentence.removeFirst();
			incomingSentence.setReturnType(QSentence::Done);
		}
		else
		if( incomingSentence.first().startsWith("!trap") )
		{
			incomingSentence.removeFirst();
			incomingSentence.setReturnType(QSentence::Trap);
		}
		else
		if( incomingSentence.first().startsWith("!fatal") )
		{
			incomingSentence.removeFirst();
			incomingSentence.setReturnType(QSentence::Fatal);
		}
		else
			incomingSentence.setReturnType(QSentence::None);
	}
}

void APICom::readBlock(QBlock &block)
{
	QSentence sentence;
	block.clear();

	do
	{
		readSentence();

		if( incomingSentence.getReturnType() != QSentence::Partial )
			block.append(incomingSentence);
	}
	while( sentence.getReturnType() == QSentence::None );
}

bool Mkt::APICom::connectTo(const QString &addr, quint16 port)
{
    if( QAbstractSocket::UnconnectedState != m_sock.state() )
	{
		emit comError(tr("Trying to connect a allready opened socket"));
        return false;
	}

    m_sock.connectToHost(m_addr = addr, m_port = port);
    return true;
}

// TODO: Poner esto en una cabecera y con un nombre más clarificador. (indica "fin de comando" )
static char *cNULL = {0};

void APICom::doLogin()
{
	switch( m_loginState )
	{
	case NoLoged:
		emit loginRequest(&m_Username, &m_Password);

		//Send login message
		writeWord("/login");
		writeWord(cNULL);
		m_loginState = LoginRequested;
		incomingSentence.clear();
		break;
	case LoginRequested:
	{
		readSentence();
		// Aun no ha llegado todo.
		if( incomingSentence.getReturnType() == QSentence::Partial )
			return;
		if( incomingSentence.getReturnType() != QSentence::Done )
		{
			emit comError(tr("Cannot login"));
			m_loginState = NoLoged;
			m_sock.close();
			break;
		}
		QSentenceMap sentMap;
		incomingSentence.getMap(sentMap);
		if( sentMap.count() != 1 )
		{
			emit comError(tr("Unknown remote login sentence format: didn't receive anything"));
			m_loginState = NoLoged;
			incomingSentence.clear();
			m_sock.close();
			break;
		}
		if( !sentMap.contains("ret") )
		{
			emit comError(tr("Unknown remote login sentence format: Doesn't receive 'ret' namefield"));
			m_loginState = NoLoged;
			incomingSentence.clear();
			m_sock.close();
			break;
		}
		if( sentMap["ret"].count() != 32 )
		{
			emit comError(tr("Unknown remote login sentence format: 'ret' field doesn't contains 32 characters"));
			m_loginState = NoLoged;
			incomingSentence.clear();
			m_sock.close();
			break;
		}
		QSentence writenSentence;

		md5_state_t state;
		md5_byte_t digest[16];

		////Place of interest: Check to see if this md5Challenge string works as as string.
		//   It may not because it needs to be binary.
		// convert szMD5Challenge to binary
		QString md5ChallengeBinary = QMD5::ToBinary(sentMap["ret"]);

		// get md5 of the password + challenge concatenation
		QMD5::init(&state);
		QMD5::append(&state, (const md5_byte_t *)cNULL, 1);
		QMD5::append(&state, (const md5_byte_t *)m_Password.toLatin1().data(),
								 strlen(m_Password.toLatin1().data()));
		QMD5::append(&state, (const md5_byte_t *)md5ChallengeBinary.toLatin1().data(), 16);
		QMD5::finish(&state, digest);

		// convert this digest to a string representation of the hex values
		// digest is the binary format of what we want to send
		// szMD5PasswordToSend is the "string" hex format
		QString md5PasswordToSend = QMD5::DigestToHexString(digest);

		// put together the login sentence
		writenSentence.append("/login");
		writenSentence.append("=name=" + m_Username);
		writenSentence.append("=response=00" + md5PasswordToSend);

		writeSentence(writenSentence);
		incomingSentence.clear();
		m_loginState = UserPassSended;
		break;
	}
	case UserPassSended:
		readSentence();
		if( incomingSentence.getReturnType() == QSentence::Partial )
			return;
		if( incomingSentence.getReturnType() == QSentence::Done )
		{
			m_loginState = LogedIn;
			incomingSentence.clear();
			emit routerListening();
		}
		else
		{
			m_loginState = NoLoged;
			incomingSentence.clear();
			m_sock.close();
			emit comError(tr("Invalid Username or Password"));
		}
		break;
	case LogedIn:
		throw "router is loged already in";
		break;
	}
}

void APICom::writeLength(int messageLength)
{
    static char encodedLengthData[4];    // encoded length to send to the api socket
    char *lengthData;           // exactly what is in memory at &iLen integer

    // set cLength address to be same as messageLength
    lengthData = (char *)&messageLength;

    // write 1 byte
    if( messageLength < 0x80 )
    {
        encodedLengthData[0] = lengthData[0];

        m_sock.write( encodedLengthData, 1 );
    }
    else
    if( messageLength < 0x4000 )// write 2 bytes
    {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
		encodedLengthData[0] = lengthData[1] | 0x80;
		encodedLengthData[1] = lengthData[0];
#else
		encodedLengthData[0] = lengthData[2] | 0x80;
		encodedLengthData[1] = lengthData[3];
#endif
        m_sock.write( encodedLengthData, 2 );
    }
    else
    if( messageLength < 0x200000)// write 3 bytes
    {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
		encodedLengthData[0] = lengthData[2] | 0xc0;
		encodedLengthData[1] = lengthData[1];
		encodedLengthData[2] = lengthData[0];
#else
		encodedLengthData[0] = lengthData[1] | 0xc0;
		encodedLengthData[1] = lengthData[2];
		encodedLengthData[2] = lengthData[3];
#endif
		m_sock.write( encodedLengthData, 3 );
    }
    else
    if( messageLength < 0x10000000 ) // write 4 bytes (untested)
    {
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
		encodedLengthData[0] = lengthData[3] | 0xe0;
		encodedLengthData[1] = lengthData[2];
		encodedLengthData[2] = lengthData[1];
		encodedLengthData[3] = lengthData[0];
#else
		encodedLengthData[0] = lengthData[0] | 0xe0;
		encodedLengthData[1] = lengthData[1];
		encodedLengthData[2] = lengthData[2];
		encodedLengthData[3] = lengthData[3];
#endif
		m_sock.write( encodedLengthData, 4 );
    }
    else
    {   // this should never happen
        printf("Length of word is %d\n", messageLength);
        printf("Word is too long.\n");
        throw "Word too long";
    }
}

/********************************************************************
 * Read a message length from the socket
 *
 * 80 = 10000000 (2 character encoded length)
 * C0 = 11000000 (3 character encoded length)
 * E0 = 11100000 (4 character encoded length)
 *
 * Message length is returned
 ********************************************************************/
// TODO: Esta función tiene un par de fallos en la
// asignación de memoria y se podría simplificar sin usar nada de ella.
int APICom::readLength()
{
	char firstChar;			// first character read from socket
	char *lengthData;		// length of next message to read...will be cast to int at the end
	int *messageLength;		// calculated length of next message (Cast to int)

	lengthData = (char *) calloc(sizeof(int), 1);

	m_sock.read( &firstChar, 1 );

	// read 4 bytes
	// this code SHOULD work, but is untested...
	if( (firstChar & 0xE0) == 0xE0 )
	{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
		lengthData[3] = firstChar;
		lengthData[3] &= 0x1f;        // mask out the 1st 3 bits
		m_sock.read( &lengthData[2], 1 );
		m_sock.read( &lengthData[1], 1 );
		m_sock.read( &lengthData[0], 1 );
#else
		lengthData[0] = firstChar;
		lengthData[0] &= 0x1f;        // mask out the 1st 3 bits
		m_sock.read( &lengthData[1], 1 );
		m_sock.read( &lengthData[2], 1 );
		m_sock.read( &lengthData[3], 1 );
#endif
		messageLength = (int *)lengthData;
	}
	else
	if( (firstChar & 0xC0) == 0xC0 ) // read 3 bytes
	{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
		lengthData[2] = firstChar;
		lengthData[2] &= 0x3f;        // mask out the 1st 2 bits
		m_sock.read( &lengthData[1], 1 );
		m_sock.read( &lengthData[0], 1 );
#else
		lengthData[1] = firstChar;
		lengthData[1] &= 0x3f;        // mask out the 1st 2 bits
		m_sock.read( &lengthData[2], 1 );
		m_sock.read( &lengthData[3], 1 );
#endif
		messageLength = (int *)lengthData;
	}
	else
	if( (firstChar & 0x80) == 0x80) // read 2 bytes
	{
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
		lengthData[1] = firstChar;
		lengthData[1] &= 0x7f;        // mask out the 1st bit
		m_sock.read( &lengthData[0], 1 );
#else
		lengthData[2] = firstChar;
		lengthData[2] &= 0x7f;        // mask out the 1st bit
		m_sock.read( &lengthData[3], 1 );
#endif
		messageLength = (int *)lengthData;
	}
	else // assume 1-byte encoded length...same on both LE and BE systems
	{
		messageLength = (int *) malloc(sizeof(int));
		*messageLength = (int)firstChar;
	}

	int retMessageLength = *messageLength;
	delete messageLength;
	delete [] lengthData;

	return retMessageLength;
}

void APICom::writeWord(const QString &strWord)
{
	writeLength( strWord.count() );
	m_sock.write( strWord.toLatin1() );
}

void Mkt::APICom::onError(QAbstractSocket::SocketError /*err*/)
{
    if( m_sock.state() != QAbstractSocket::ConnectedState )
        emit comConnected(false);
	emit comError(m_sock.errorString());
}

void APICom::onConnected()
{
    emit comConnected(true);
	m_loginState = NoLoged;
	doLogin();
}

void APICom::onDisconnected()
{
	emit comConnected(false);
}

void APICom::onHostLookup()
{
	emit addrFound();
}

void APICom::onReadyRead()
{
	// TODO: Aquí debo comprobar que esté todo el mensaje desde el router antes de
	// notificar que se puede leer del socket.
	if( m_loginState != LogedIn )
		doLogin();
	else
		emit comReceive();
}
