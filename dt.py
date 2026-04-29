import glob
import os
import pandas as pd
from sklearn.tree import DecisionTreeClassifier, export_text
from micromlgen import port


def load_and_label_data(data_dir='./data'):
	files = sorted(glob.glob(os.path.join(data_dir, 'data_*.csv')))
	if not files:
		raise FileNotFoundError(f'No CSV files matching data_*.csv in {data_dir}')

	dfs = []
	expected_feature_count = 28
	for f in files:
		df = pd.read_csv(f, header=None)
		ncols = df.shape[1]
		if ncols == expected_feature_count:
			colnames = ['d_t'] + [f'v_t_{i}' for i in range(1, 15)] + [f'a_t_{i}' for i in range(1, 14)]
			df.columns = colnames
		elif ncols == 30:
			colnames = ['d_t'] + ['v_t'] + [f'v_t_{i}' for i in range(1, 15)] + ['a_t'] + [f'a_t_{i}' for i in range(1, 14)]
			df.columns = colnames
		else:
			df.columns = [f'f{i}' for i in range(ncols)]

		# label by filename: data_1_*.csv -> 1, data_0_*.csv -> 0
		basename = os.path.basename(f)
		label = 1 if basename.startswith('data_1_') else 0
		df['target'] = label
		dfs.append(df)

	combined = pd.concat(dfs, ignore_index=True)
	combined.fillna(0, inplace=True)
	return combined


if __name__ == '__main__':
	df = load_and_label_data('./data')

	if 'target' not in df.columns:
		raise RuntimeError('No target column present after loading data')

	# Truncate to at most 500 samples per class to balance the dataset
	max_per_class = 500
	samples = []
	for label in [0, 1]:
		grp = df[df['target'] == label]
		if grp.empty:
			print(f'Warning: no samples found for label {label}')
			continue
		if len(grp) > max_per_class:
			grp = grp.sample(n=max_per_class, random_state=42)
		samples.append(grp)

	if samples:
		df = pd.concat(samples, ignore_index=True)
		# Shuffle rows so positives and negatives are randomized
		df = df.sample(frac=1, random_state=42).reset_index(drop=True)
	else:
		raise RuntimeError('No samples available after truncation')

	# Diagnostics: show loaded dataframe size and a preview of first 10 columns
	print('DataFrame shape:', df.shape)
	print('Preview (first 10 columns):')
	print(df.iloc[:, :10].head().to_string(index=False))

	X = df.drop('target', axis=1)
	y = df['target']

	tree_model = DecisionTreeClassifier(max_depth=3, random_state=42)
	tree_model.fit(X, y)

	print(f'Loaded {df.shape[0]} rows with {df.shape[1]} columns (including target)')
	print(export_text(tree_model, feature_names=list(X.columns)))

	header_path = os.path.join('include', 'gate_model.h')
	with open(header_path, 'w', encoding='utf-8') as header_file:
		header_file.write(port(tree_model))
	print(f'Wrote C++ model header to {header_path}')