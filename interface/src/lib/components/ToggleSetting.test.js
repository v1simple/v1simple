import { fireEvent, render, screen } from '@testing-library/svelte';
import { describe, expect, it, vi } from 'vitest';
import ToggleSetting from './ToggleSetting.svelte';

describe('ToggleSetting', () => {
	it('renders the label and description', () => {
		render(ToggleSetting, {
			title: 'Show Battery Percent',
			description: 'Expose the percent badge next to the icon.',
			checked: true
		});

		expect(screen.getByText('Show Battery Percent')).toBeInTheDocument();
		expect(screen.getByText('Expose the percent badge next to the icon.')).toBeInTheDocument();
		expect(screen.getByRole('checkbox', { name: /show battery percent/i })).toBeChecked();
	});

	it('forwards checkbox changes through the callback prop', async () => {
		const onChange = vi.fn();
		render(ToggleSetting, {
			title: 'Hide BLE Icon',
			checked: false,
			onChange
		});

		await fireEvent.click(screen.getByRole('checkbox', { name: /hide ble icon/i }));
		expect(onChange).toHaveBeenCalledTimes(1);
		expect(onChange).toHaveBeenCalledWith(true);
	});
});
