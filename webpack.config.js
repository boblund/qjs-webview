// webpack.config.js
const path = require( 'path' );

module.exports = ( env = {} ) => {
	const isQuickJS = !!env.quickjs;

	return {
		mode: 'development', //'production'
		devtool: 'source-map', // false, //
		entry: './index.mjs',
		output: {
			filename: 'bundle.js',
			publicPath: '',        // relative, not absolute-from-root — matters for file:// resolution
			// do NOT set `module: true` here — that switches output to real ESM
		},
		target: 'web',
		module: {
			rules: [
				{
					test: /\.css$/,
					resourceQuery: /stylesheet/,
					use: [
						{
							loader: 'css-loader',
							options: {
								exportType: 'css-style-sheet',
								esModule: true
							}
						}
					]
				},
				{
					test: /\.css$/,
					resourceQuery: { not: [ /stylesheet/ ] },
					use: [ 'style-loader', 'css-loader' ],
				},
				{
					test: /.(mjs|js)$/,
					use: [
						{ loader: "ifdef-loader", options: {
							QUICKJS: isQuickJS,
							WEBPACK: true,
							"ifdef-uncomment-prefix": "// #code "
						} }
					]
				},
				{
					test: /\.js$/,
					exclude: /(node_modules)/,
					use: {
						loader: "babel-loader",
					}
				}
			]
		}
	};
};
