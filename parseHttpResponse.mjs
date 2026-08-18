export { initHttpResponse };

function initHttpResponse( doneFunc, noLengthCb = undefined ) {
	let responseState = 'header'; // 'chunked', 'contentLength', 'done'
	let parsedHeaders = null;
	let bodyExpected  = -1;
	let done = doneFunc;

	return function( _readBuf ){
		if( responseState != 'contentLength' && _readBuf.length == 0 ) return;
		let readBuf = _readBuf.slice( 0 );

		noOverflow: while( true ){
			let response, overflow;
			switch( responseState ){
				case 'header':
					if( ( response = parseHeader( readBuf, noLengthCb ) ) ){
						if( response?.error ){
							done( { error: response.error } );
							responseState = 'done';
							break noOverflow;
						}
						( { parsedHeaders, overflow, bodyExpected } = response );
						responseState = bodyExpected == undefined ? 'chunked' : 'contentLength';
						readBuf = overflow;
						break;
					}
					break noOverflow;

				case 'chunked':
					if( ( response = parseChunked( readBuf ) ) ){
						responseState = 'done';
						done( { headers: parsedHeaders, body: response.body } );
					}
					break noOverflow;

				case 'contentLength':
					if( ( response = parseContentLength( readBuf, bodyExpected  ) ) ){
						responseState = 'done';
						done( { headers: parsedHeaders, body: response.body } );
					}
					break noOverflow;
			}
		}
	};
}

function parseHeader( readBuf, noLengthCb ){
	let headerBuf			= new Uint8Array();

	parseHeader = function( readBuf ){
		headerBuf = uint8Concat( headerBuf, readBuf );
		const sep = uint8FindSeq( headerBuf, [ 13, 10, 13, 10 ] );
		if ( sep === -1 ) return;
		let parsedHeaders   = uint8ToString( headerBuf.slice( 0, sep ) );
		const overflow = headerBuf.slice( sep + 4 );
		headerBuf	= new Uint8Array();
		const match = parsedHeaders.match( /content-length:\s*(\d+)/i );
		let bodyExpected = undefined;
		if( match ){
			bodyExpected = parseInt( match[1], 10 );
		}else if( !( /transfer-encoding:\s*chunked/i.test( parsedHeaders ) ) ){
			if( noLengthCb && Number( parsedHeaders.match( /HTTP\/(.*?)\s+.*/ )[ 1 ] ) < 1.1 ){
				noLengthCb();
				bodyExpected = -1;
			} else {
				return { error: 'no Content-Length and not chunked' };
			}
		}
		return { parsedHeaders, overflow, bodyExpected };
	};

	return parseHeader( readBuf );
}

function parseChunked( tmp ){
	let bodyExpected = -1;
	let bodyReceived = 0;
	let bodyChunks = [];
	let bodyBuf = new Uint8Array( 0 );
	let chunkState = "CHUNK_SIZE";
	let chunkRemaining = 0;

	parseChunked = function( tmp  ){
		bodyBuf = uint8Concat( bodyBuf, tmp );
		loop: while ( true ) {
			switch ( chunkState ) {
				case "CHUNK_SIZE": {
					const crlfIdx = uint8FindSeq( bodyBuf, [ 13, 10 ] );
					if ( crlfIdx === -1 ) return;
					const sizeLine = uint8ToString( bodyBuf, crlfIdx );
					chunkRemaining = parseInt( sizeLine.split( ";" )[0].trim(), 16 );
					bodyBuf = bodyBuf.slice( crlfIdx + 2 );
					if ( chunkRemaining === 0 ) { chunkState = "DONE"; continue; }
					bodyExpected = bodyReceived + chunkRemaining;
					chunkState   = "CHUNK_DATA";
					break;
				}

				case "CHUNK_DATA": {
					const take    = Math.min( bodyBuf.length, bodyExpected - bodyReceived );
					if ( take === 0 ) return;
					bodyChunks.push( bodyBuf.slice( 0, take ) );
					bodyBuf        = bodyBuf.slice( take );
					bodyReceived += take;
					if( bodyReceived < bodyExpected ) return; // chunk incomplete
					chunkRemaining = 0;
					chunkState     = "CHUNK_CRLF";
					break;
				}

				case "CHUNK_CRLF":
					if ( bodyBuf.length < 2 ){
						chunkState = 'DONE';
					} else {
						bodyBuf     = bodyBuf.slice( 2 );
						chunkState = "CHUNK_SIZE";
					}
					break;

				case "DONE":
					chunkState = 'CHUNK_SIZE';
					break loop;
			}
		}

		return { body: uint8ToString( mergeChunks( bodyChunks, bodyExpected ) ) };
	};

	return parseChunked( tmp );
}

function parseContentLength( tmp, bodyExpected ){
	let bodyReceived  = 0;
	let bodyChunks    = [];
	parseContentLength = function( tmp ){
		if( tmp.length == 0 ){
			// http 1.0 no content-length
			return { body: uint8ToString( mergeChunks( bodyChunks, bodyReceived ) ) };
		}
		const take = bodyExpected == -1 ? tmp.length : Math.min( tmp.length, bodyExpected - bodyReceived );
		bodyChunks.push( tmp.slice( 0, take ) );
		bodyReceived += tmp.length;
		if ( bodyExpected !== -1 && bodyReceived >= bodyExpected ) {
			return { body: uint8ToString( mergeChunks( bodyChunks, bodyExpected ) ) };
		}
	};

	return parseContentLength( tmp, bodyExpected );
}

// Helpers

function uint8Concat( a, b ) {
	const out = new Uint8Array( a.length + b.length );
	out.set( a, 0 );
	out.set( b, a.length );
	return out;
}

function uint8ToString( buf, len = 0 ) {
	return String.fromCharCode( ...( len == 0 ? buf : buf.slice( 0, len ) ) );
}

function mergeChunks( chunks, totalBytes ) {
	const result = new Uint8Array( Math.max( 0, totalBytes ) );
	let offset = 0;
	for ( const chunk of chunks ) {
		result.set( chunk, offset );
		offset += chunk.length;
	}
	return result;
}

function uint8FindSeq( uint8Buf, seq ) {
	outer: for ( let i = 0; i <= uint8Buf.length - seq.length; i++ ) {
		for ( let j = 0; j < seq.length; j++ )
			if ( uint8Buf[i + j] !== seq[j] ) continue outer;
		return i;
	}
	return -1;
}