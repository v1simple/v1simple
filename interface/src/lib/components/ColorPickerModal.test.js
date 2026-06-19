import { fireEvent, render, screen, within } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';

import ColorPickerModal from './ColorPickerModal.svelte';

function renderOpen(props = {}) {
	return render(ColorPickerModal, {
		open: true,
		label: 'Band Ka Color',
		red: 8,
		green: 16,
		blue: 24,
		oncancel: vi.fn(),
		onapply: vi.fn(),
		...props
	});
}

describe('ColorPickerModal', () => {
	it('does not render when closed', () => {
		render(ColorPickerModal, { open: false, label: 'Hidden picker' });

		expect(screen.queryByText('Hidden picker')).not.toBeInTheDocument();
	});

	it('renders the selected color and updates channel labels from range input', async () => {
		renderOpen();

		expect(screen.getByText('Band Ka Color')).toBeInTheDocument();
		const preview = document.querySelector('.surface-color-preview');
		expect(preview).not.toBeNull();
		expect(preview).toHaveStyle({ backgroundColor: 'rgb(8, 16, 24)' });

		const redControl = screen.getByLabelText(/red/i);
		await fireEvent.input(redControl, { target: { value: '248' } });

		const redField = redControl.closest('.field-control');
		expect(within(redField).getByText('248')).toBeInTheDocument();
	});

	it('applies quick presets and forwards cancel/apply actions', async () => {
		const oncancel = vi.fn();
		const onapply = vi.fn();
		renderOpen({ oncancel, onapply });

		await fireEvent.click(screen.getByRole('button', { name: 'Blue' }));
		expect(screen.getByLabelText(/blue/i)).toHaveValue('248');

		await fireEvent.click(screen.getByRole('button', { name: /apply/i }));
		expect(onapply).toHaveBeenCalledTimes(1);

		await fireEvent.click(screen.getByRole('button', { name: /cancel/i }));
		expect(oncancel).toHaveBeenCalledTimes(1);
	});

	it('closes from the keyboard-accessible backdrop', async () => {
		const oncancel = vi.fn();
		renderOpen({ oncancel });

		const backdrop = screen.getByRole('button', { name: /close color picker/i });
		await fireEvent.keyDown(backdrop, { key: 'Enter' });
		expect(oncancel).toHaveBeenCalledTimes(1);
	});
});
